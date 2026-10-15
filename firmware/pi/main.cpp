// ridgelined: Ridgeline node daemon for a Raspberry Pi with the Backcountry
// Signal HAT (E22P-915M30S / SX1262 + MAX-M10S GPS).
//
// Default pins (BCM) match the HAT and Meshtastic's lora-pimesh-1w-v2 preset:
//   CS 8 (CE0)  DIO1 6  BUSY 5  RESET 18  module EN 26 (held high in config.txt)
//
// Usage: sudo ./ridgelined [--id HEX] [--freq MHz] [--power dBm] [--gps /dev/serial0]
//                          [--lat L --lon L]  (fixed position, no GPS)
//                          [--key FILE] [--psk FILE] [--peers FILE]
//                          [--require-enc] [--responder]
// stdin commands:  send <hex-id> <text> | known <hex-id> <lat> <lon> | stats | pos
//                  keys | peer <hex-id> <hex-pubkey>
//                  sos <text> | sosstop | sosack <hex-id>
//
// Key files (DESIGN §9.1):
//   --key    X25519 identity secret, 64 hex chars (or 32 raw bytes). Created with
//            mode 600 if missing. Default /var/lib/ridgeline/identity.key.
//   --psk    optional channel key, same format. Generate: openssl rand -hex 32
//   --peers  optional "<hex-id> <hex-pubkey>" lines ('#' comments), as printed
//            by the `keys` command on the other node.
#include <RadioLib.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "hal/RPi/PiHal.h"
#include "ridgeline/crypto.h"
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
  std::string keyFile = "/var/lib/ridgeline/identity.key";
  std::string pskFile, peersFile;
  bool requireEnc = false, responder = false;
};

using Key32 = std::array<uint8_t, rl::kKeySize>;

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

std::string toHex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool fromHex(const std::string& s, uint8_t* out, size_t n) {
  if (s.size() != 2 * n) return false;
  for (size_t i = 0; i < n; ++i) {
    int hi = hexNibble(s[2 * i]), lo = hexNibble(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

bool parseId(const std::string& s, uint32_t& id) {
  if (s.empty() || s.size() > 8) return false;
  char* end = nullptr;
  id = static_cast<uint32_t>(std::strtoul(s.c_str(), &end, 16));
  return *end == '\0' && id != 0;
}

// Reads a 32-byte key: exactly 32 raw bytes, or 64 hex chars plus optional
// whitespace. The file buffer is wiped before returning.
bool readKeyFile(const std::string& path, Key32& key) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  char buf[160];
  ssize_t n = read(fd, buf, sizeof buf);
  close(fd);
  bool ok = false;
  if (n == static_cast<ssize_t>(rl::kKeySize)) {
    std::memcpy(key.data(), buf, rl::kKeySize);
    ok = true;
  } else if (n > 0) {
    std::string hex;
    for (ssize_t i = 0; i < n; ++i)
      if (!std::isspace(static_cast<unsigned char>(buf[i]))) hex += buf[i];
    ok = fromHex(hex, key.data(), key.size());
    rl::wipe(&hex[0], hex.size());
  }
  rl::wipe(buf, sizeof buf);
  return ok;
}

bool randomBytes(uint8_t* out, size_t n) {
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return false;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, out + got, n - got);
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  close(fd);
  return got == n;
}

// Loads the identity secret, creating it (mode 600, parent dir 700) if missing.
bool loadIdentity(const std::string& path, Key32& secret) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    if (st.st_mode & (S_IRWXG | S_IRWXO))
      std::fprintf(stderr, "warning: %s is readable by others; chmod 600 it\n", path.c_str());
    if (readKeyFile(path, secret)) return true;
    std::fprintf(stderr, "bad identity key in %s (want 64 hex chars)\n", path.c_str());
    return false;
  }
  size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) mkdir(path.substr(0, slash).c_str(), 0700);
  if (!randomBytes(secret.data(), secret.size())) {
    std::fprintf(stderr, "cannot read /dev/urandom\n");
    return false;
  }
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    std::fprintf(stderr, "cannot create %s: %s\n", path.c_str(), std::strerror(errno));
    return false;
  }
  fchmod(fd, 0600);   // in case the umask was odd
  std::string hex = toHex(secret.data(), secret.size()) + "\n";
  bool ok = write(fd, hex.data(), hex.size()) == static_cast<ssize_t>(hex.size());
  ok = (fsync(fd) == 0) && ok;
  close(fd);
  rl::wipe(&hex[0], hex.size());
  if (!ok) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    unlink(path.c_str());
    return false;
  }
  std::fprintf(stderr, "generated new identity key %s\n", path.c_str());
  return true;
}

bool addPeer(rl::Router& r, const std::string& idHex, const std::string& pubHex) {
  uint32_t id;
  uint8_t pub[rl::kKeySize];
  if (!parseId(idHex, id) || !fromHex(pubHex, pub, sizeof pub)) return false;
  return r.addPeerKey(id, pub);
}

// One "<hex-id> <hex-pubkey>" per line; blank lines and '#' comments skipped.
// Returns the number of keys added, or -1 if the file cannot be opened.
int loadPeers(const std::string& path, rl::Router& r) {
  std::ifstream f(path);
  if (!f) return -1;
  std::string line;
  int n = 0, lineNo = 0;
  while (std::getline(f, line)) {
    ++lineNo;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    std::istringstream in(line);
    std::string id, pub;
    if (!(in >> id)) continue;
    in >> pub;
    if (addPeer(r, id, pub)) ++n;
    else std::fprintf(stderr, "%s:%d: bad peer line (want <hex-id> <64 hex chars>)\n", path.c_str(), lineNo);
  }
  return n;
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
  void onSos(const rl::SosInfo& s) override {
    std::printf("SOS from %08x seq %u", s.src, s.seq);
    if (s.pos.valid) std::printf(" at %.6f %.6f", s.pos.lat, s.pos.lon);
    else std::printf(" no fix");
    if (s.accuracy != 0xFF) std::printf(" +-%um", s.accuracy * 4u);
    if (s.battery != 0xFF) std::printf(" batt %u%%", s.battery);
    std::printf(": %s\n", s.text.c_str());
    std::printf("  (ack with: sosack %08x)\n", s.src);
    std::fflush(stdout);
  }
  void onSosAck(uint32_t responder, uint16_t seq) override {
    std::printf("SOSACK from %08x for seq %u\n", responder, seq);
    std::fflush(stdout);
  }
};

void handleCommand(const std::string& line, rl::Router& r, const rl::Position& pos,
                   const Key32& pub) {
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
    std::printf("tx %u rx %u bad %u authfail %u delivered %u forwarded %u stored %u air %llums "
                "store %zu nb %zu sos held %zu sent %u heard %u%s\n",
                s.tx, s.rx, s.rxBad, s.rxAuthFail, s.delivered, s.forwarded, s.stored,
                static_cast<unsigned long long>(s.airMs), r.storeSize(), r.neighborCount(nowMs()),
                r.sosCount(), s.sosTx, s.sosHeard, r.sosActive() ? " SOS-ACTIVE" : "");
  } else if (cmd == "pos") {
    std::printf("%s %.6f %.6f\n", pos.valid ? "fix" : "nofix", pos.lat, pos.lon);
  } else if (cmd == "keys") {
    // same format as a --peers line, so it can be pasted on the other node
    std::printf("%08x %s\n", r.id(), toHex(pub.data(), pub.size()).c_str());
  } else if (cmd == "peer") {
    std::string id, hex;
    in >> id >> hex;
    std::printf(addPeer(r, id, hex) ? "ok\n" : "bad peer (want <hex-id> <64 hex chars>)\n");
  } else if (cmd == "sos") {
    std::string text;
    std::getline(in, text);
    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
    if (r.startSos(text, 0xFF, 0xFF, nowMs()))
      std::printf("SOS on%s, repeating until sosstop\n", text.size() > rl::kSosTextMax ? " (text cut to 60 bytes)" : "");
    else
      std::printf("SOS rate limited, try again in a minute\n");
  } else if (cmd == "sosstop") {
    r.stopSos();
    std::printf("SOS off\n");
  } else if (cmd == "sosack") {
    std::string id;
    in >> id;
    uint32_t src;
    if (!parseId(id, src)) std::printf("usage: sosack <hex-id>\n");
    else if (r.ackSos(src, nowMs())) std::printf("acked SOS from %08x\n", src);
    else std::printf("no SOS held from %08x\n", src);
  } else if (!cmd.empty()) {
    std::printf("commands: send <hex-id> <text> | known <hex-id> <lat> <lon> | stats | pos\n"
                "          keys | peer <hex-id> <hex-pubkey> | sos <text> | sosstop | sosack <hex-id>\n");
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
    else if (a == "--key") o.keyFile = next();
    else if (a == "--psk") o.pskFile = next();
    else if (a == "--peers") o.peersFile = next();
    else if (a == "--require-enc") o.requireEnc = true;
    else if (a == "--responder") o.responder = true;
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  if (!o.id) o.id = machineId();
  g_irqPin = o.pinIrq;

  // Keys first: no point bringing the radio up if they are unusable. The
  // Router does not touch the radio until onReceive / tick.
  Key32 secret{}, pub{}, psk{};
  if (!loadIdentity(o.keyFile, secret)) return 1;
  rl::x25519Public(secret.data(), pub.data());
  bool havePsk = false;
  if (!o.pskFile.empty()) {
    if (!readKeyFile(o.pskFile, psk)) {
      std::fprintf(stderr, "bad channel key in %s (want 64 hex chars or 32 raw bytes)\n", o.pskFile.c_str());
      return 1;
    }
    havePsk = true;
  }

  rl::Config cfg;
  cfg.nodeId = o.id;
  cfg.requireEncryption = o.requireEnc;
  cfg.responder = o.responder;
  PiRadio radio;
  StdoutApp app;
  rl::Router router(cfg, radio, app);
  router.setIdentityKey(secret.data());
  rl::wipe(secret.data(), secret.size());
  if (havePsk) router.setChannelKey(psk.data());
  rl::wipe(psk.data(), psk.size());
  int peers = 0;
  if (!o.peersFile.empty()) {
    peers = loadPeers(o.peersFile, router);
    if (peers < 0) {
      std::fprintf(stderr, "cannot open peers file %s\n", o.peersFile.c_str());
      return 1;
    }
  }

  g_hal = new PiHal(0);   // SPI0, CE0 = BCM8
  g_radio = new SX1262(new Module(g_hal, o.pinCs, o.pinIrq, o.pinRst, o.pinBusy));
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

  rl::Position pos;
  if (o.fixedPos) {
    pos = {o.lat, o.lon, true};
    router.setPosition(pos, nowMs());
  }
  int gps = o.fixedPos ? -1 : openGps(o.gpsDev);
  if (!o.fixedPos && gps < 0) std::fprintf(stderr, "warning: cannot open %s, no position\n", o.gpsDev.c_str());

  std::printf("ridgelined node %08x on %.3f MHz, %d dBm drive\n", o.id, o.freqMHz, o.powerDbm);
  std::printf("key %s, channel key %s, %d peer key%s%s%s\n", toHex(pub.data(), pub.size()).c_str(),
              havePsk ? "on" : "off", peers, peers == 1 ? "" : "s",
              o.requireEnc ? ", plaintext DATA refused" : "", o.responder ? ", SOS responder" : "");
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
          handleCommand(cmdLine, router, pos, pub);
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
