// ridgelined: Ridgeline node daemon for a Raspberry Pi with the Backcountry
// Signal HAT (E22P-915M30S / SX1262 + MAX-M10S GPS).
//
// Default pins (BCM) match the HAT and Meshtastic's lora-pimesh-1w-v2 preset:
//   CS 8 (CE0)  DIO1 6  BUSY 5  RESET 18  module EN 26 (held high in config.txt)
//
// Usage: sudo ./ridgelined [--id HEX] [--freq MHz] [--power dBm] [--gps /dev/serial0]
//                          [--lat L --lon L]  (fixed position, no GPS)
// stdin commands:  send <hex-id> <text> | known <hex-id> <lat> <lon> | stats | pos
#include <RadioLib.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "hal/RPi/PiHal.h"
#include "ridgeline/router.h"

namespace {

struct Options {
  uint32_t id = 0;
  float freqMHz = 914.875f;     // clear of Meshtastic/MeshCore US defaults
  int8_t powerDbm = 10;         // SX1262 drive; the E22P PA adds ~8–10 dB. Check your rules.
  uint8_t syncWord = 0x24;      // private: keeps other meshes' frames out at the PHY
  std::string gpsDev = "/dev/serial0";
  double lat = 0, lon = 0;
  bool fixedPos = false;
  int pinCs = 8, pinIrq = 6, pinRst = 18, pinBusy = 5;
};

uint64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t machineId() {
  std::ifstream f("/etc/machine-id");
  std::string s;
  std::getline(f, s);
  if (s.empty()) return 0x12345678;
  return rl::crc32c(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

int openGps(const std::string& dev) {
  int fd = open(dev.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -1;
  termios t{};
  tcgetattr(fd, &t);
  cfmakeraw(&t);
  cfsetispeed(&t, B9600);   // MAX-M10S factory default
  cfsetospeed(&t, B9600);
  tcsetattr(fd, TCSANOW, &t);
  return fd;
}

PiHal* g_hal = nullptr;
SX1262* g_radio = nullptr;
int g_irqPin = 6;

class PiRadio : public rl::RadioPort {
 public:
  bool channelBusy() override {
    if (g_hal->digitalRead(g_irqPin)) return true;      // a packet is waiting
    int16_t r = g_radio->scanChannel();                  // CAD
    g_radio->startReceive();
    return r == RADIOLIB_LORA_DETECTED;
  }
  bool transmit(const uint8_t* buf, size_t len) override {
    int16_t st = g_radio->transmit(const_cast<uint8_t*>(buf), len);
    g_radio->startReceive();
    if (st != RADIOLIB_ERR_NONE) std::fprintf(stderr, "tx failed: %d\n", st);
    return st == RADIOLIB_ERR_NONE;
  }
};

class StdoutApp : public rl::AppPort {
 public:
  void onMessage(uint32_t src, uint32_t msgId, const std::vector<uint8_t>& p) override {
    std::printf("MSG from %08x id %08x: %.*s\n", src, msgId, static_cast<int>(p.size()),
                reinterpret_cast<const char*>(p.data()));
    std::fflush(stdout);
  }
  void onDelivered(uint32_t dst, uint32_t msgId) override {
    std::printf("DELIVERED to %08x id %08x\n", dst, msgId);
    std::fflush(stdout);
  }
};

void handleCommand(const std::string& line, rl::Router& r, const rl::Position& pos) {
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  if (cmd == "send") {
    std::string id, text;
    in >> id;
    std::getline(in, text);
    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
    uint32_t dst = std::strtoul(id.c_str(), nullptr, 16);
    uint32_t mid = r.send(dst, std::vector<uint8_t>(text.begin(), text.end()), nowMs());
    std::printf("queued id %08x -> %08x\n", mid, dst);
  } else if (cmd == "known") {
    std::string id;
    double la, lo;
    in >> id >> la >> lo;
    r.addKnownPosition(std::strtoul(id.c_str(), nullptr, 16), {la, lo, true}, nowMs());
    std::printf("ok\n");
  } else if (cmd == "stats") {
    auto& s = r.stats();
    std::printf("tx %u rx %u bad %u delivered %u forwarded %u stored %u air %llums store %zu nb %zu\n",
                s.tx, s.rx, s.rxBad, s.delivered, s.forwarded, s.stored,
                static_cast<unsigned long long>(s.airMs), r.storeSize(), r.neighborCount(nowMs()));
  } else if (cmd == "pos") {
    std::printf("%s %.6f %.6f\n", pos.valid ? "fix" : "nofix", pos.lat, pos.lon);
  } else if (!cmd.empty()) {
    std::printf("commands: send <hex-id> <text> | known <hex-id> <lat> <lon> | stats | pos\n");
  }
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--id") o.id = std::strtoul(next(), nullptr, 16);
    else if (a == "--freq") o.freqMHz = std::atof(next());
    else if (a == "--power") o.powerDbm = static_cast<int8_t>(std::atoi(next()));
    else if (a == "--gps") o.gpsDev = next();
    else if (a == "--lat") { o.lat = std::atof(next()); o.fixedPos = true; }
    else if (a == "--lon") { o.lon = std::atof(next()); o.fixedPos = true; }
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  if (!o.id) o.id = machineId();
  g_irqPin = o.pinIrq;

  g_hal = new PiHal(0);   // SPI0, CE0 = BCM8
  g_radio = new SX1262(new Module(g_hal, o.pinCs, o.pinIrq, o.pinRst, o.pinBusy));
  rl::Config cfg;
  cfg.nodeId = o.id;
  // "Trail" preset: SF9 / BW125 / CR4/5 / 16-symbol preamble. E22 uses a 1.8 V TCXO.
  int16_t st = g_radio->begin(o.freqMHz, 125.0, cfg.phy.sf, 5, o.syncWord, o.powerDbm,
                              cfg.phy.preamble, 1.8, false);
  if (st != RADIOLIB_ERR_NONE) {
    std::fprintf(stderr, "radio init failed: %d (check SPI enabled, EN pin high, wiring)\n", st);
    return 1;
  }
  g_radio->setDio2AsRfSwitch(true);
  g_radio->setCRC(2);
  g_radio->startReceive();

  PiRadio radio;
  StdoutApp app;
  rl::Router router(cfg, radio, app);
  rl::Position pos;
  if (o.fixedPos) {
    pos = {o.lat, o.lon, true};
    router.setPosition(pos, nowMs());
  }
  int gps = o.fixedPos ? -1 : openGps(o.gpsDev);
  if (!o.fixedPos && gps < 0) std::fprintf(stderr, "warning: cannot open %s, no position\n", o.gpsDev.c_str());

  std::printf("ridgelined node %08x on %.3f MHz, %d dBm drive\n", o.id, o.freqMHz, o.powerDbm);
  std::fflush(stdout);

  std::string gpsLine, cmdLine;
  uint8_t buf[256];
  for (;;) {
    uint64_t now = nowMs();
    if (g_hal->digitalRead(o.pinIrq)) {
      size_t len = g_radio->getPacketLength();
      if (len > 0 && len <= sizeof buf && g_radio->readData(buf, len) == RADIOLIB_ERR_NONE)
        router.onReceive(buf, len, g_radio->getRSSI(), now);
      g_radio->startReceive();
    }
    if (gps >= 0) {
      char c[128];
      ssize_t n = read(gps, c, sizeof c);
      for (ssize_t i = 0; i < n; ++i) {
        if (c[i] == '\n') {
          rl::Position p;
          if (rl::parseNmea(gpsLine, p)) {
            pos = p;
            router.setPosition(pos, now);
          }
          gpsLine.clear();
        } else if (gpsLine.size() < 120) {
          gpsLine += c[i];
        }
      }
    }
    pollfd pf{STDIN_FILENO, POLLIN, 0};
    if (poll(&pf, 1, 0) > 0) {
      char c[256];
      ssize_t n = read(STDIN_FILENO, c, sizeof c);
      for (ssize_t i = 0; i < n; ++i) {
        if (c[i] == '\n') {
          handleCommand(cmdLine, router, pos);
          cmdLine.clear();
        } else {
          cmdLine += c[i];
        }
      }
    }
    router.tick(now);
    usleep(2000);
  }
}
