// GhostWave node firmware for ESP32-S3 LoRa boards (Heltec Wireless Tracker
// V2, Heltec V3). Same router as ghostwaved; the console is a line CLI on the
// USB serial port (115200 baud) with ghostwaved's commands plus a few that
// replace its command-line options, persisted in NVS. Type `help`.
#include <Arduino.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "board.h"
#include "ghostwave/crypto.h"
#include "ghostwave/router.h"
#include "ghostwave/util.h"
#include "radio.h"
#include "settings.h"

namespace {

using gw::mcu::Key32;

// Table sizes. storeCapacity / sosCapacity are the core defaults; the seen
// table is unbounded on the Pi and capped here. loc_ / nb_ are capped at 512
// inside the core.
constexpr size_t kSeenCapacity = 512;
constexpr size_t kLocCapacity = 512;
constexpr size_t kPeerCapacity = 64;

// Worst-case router heap with every table full, for a 32-bit target. Rough by
// design: each std::map/std::set node is costed at 16 B of tree links + 8 B of
// allocator overhead, and every stored frame at a full kMaxFrame payload.
constexpr size_t kNode = 24;
constexpr size_t kPacketHeap = sizeof(gw::Packet) + gw::kMaxFrame + 8;
size_t worstCaseTableBytes(const gw::Config& c) {
  size_t store = c.storeCapacity * (kNode + 12 + kPacketHeap + 40 + 8 * kNode);  // + `given` set
  size_t seen = c.seenCapacity * (kNode + 12 + 48);
  size_t loc = kLocCapacity * (kNode + 4 + sizeof(gw::Position) + 8);
  size_t nb = loc;
  size_t sos = c.sosCapacity * (kNode + 4 + kPacketHeap + 40 + 4 * kNode);
  size_t peers = kPeerCapacity * (kNode + 4 + 32);
  // TX queue, pending forwards, custody/ack waits: bounded in practice by the
  // store and the airtime budget; allow 48 frames in flight.
  size_t inflight = 48 * (kNode + 12 + 2 * kPacketHeap + 64);
  return store + seen + loc + nb + sos + peers + inflight;
}

uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

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

bool parseDouble(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return *end == '\0';
}

// Splits off the first space-separated word of `rest`.
std::string word(std::string& rest) {
  size_t a = rest.find_first_not_of(' ');
  if (a == std::string::npos) {
    rest.clear();
    return {};
  }
  size_t b = rest.find(' ', a);
  std::string w = rest.substr(a, b == std::string::npos ? std::string::npos : b - a);
  rest = b == std::string::npos ? std::string() : rest.substr(b + 1);
  return w;
}

class SerialApp : public gw::AppPort {
 public:
  void onMessage(uint32_t src, uint32_t msgId, const std::vector<uint8_t>& p) override {
    Serial.printf("MSG from %08x id %08x: %.*s\n", src, msgId, static_cast<int>(p.size()),
                  reinterpret_cast<const char*>(p.data()));
  }
  void onDelivered(uint32_t dst, uint32_t msgId) override {
    Serial.printf("DELIVERED to %08x id %08x\n", dst, msgId);
  }
  void onSos(const gw::SosInfo& s) override {
    Serial.printf("SOS from %08x seq %u", s.src, s.seq);
    if (s.pos.valid) Serial.printf(" at %.6f %.6f", s.pos.lat, s.pos.lon);
    else Serial.print(" no fix");
    if (s.accuracy != 0xFF) Serial.printf(" +-%um", s.accuracy * 4u);
    if (s.battery != 0xFF) Serial.printf(" batt %u%%", s.battery);
    Serial.printf(": %s\n", s.text.c_str());
    Serial.printf("  (ack with: sosack %08x)\n", s.src);
  }
  void onSosAck(uint32_t responder, uint16_t seq) override {
    Serial.printf("SOSACK from %08x for seq %u\n", responder, seq);
  }
};

gw::mcu::Settings g_set;
gw::mcu::McuRadio g_radio;
SerialApp g_app;
gw::Config g_cfg;
std::unique_ptr<gw::Router> g_router;
Key32 g_pub{};
gw::Position g_pos;          // what the router was last given
bool g_gpsFix = false;
#if GW_HAS_GPS
uint64_t g_gpsFixMs = 0;
uint32_t g_nmeaGood = 0;
#endif
bool g_radioOk = false;
size_t g_tableBudget = 0;
std::string g_cmdLine, g_gpsLine;

void printStats() {
  auto& s = g_router->stats();
  Serial.printf("tx %u rx %u bad %u authfail %u delivered %u forwarded %u stored %u air %llums "
                "store %u nb %u sos held %u sent %u heard %u%s\n",
                s.tx, s.rx, s.rxBad, s.rxAuthFail, s.delivered, s.forwarded, s.stored,
                static_cast<unsigned long long>(s.airMs), static_cast<unsigned>(g_router->storeSize()),
                static_cast<unsigned>(g_router->neighborCount(nowMs())), static_cast<unsigned>(g_router->sosCount()),
                s.sosTx, s.sosHeard, g_router->sosActive() ? " SOS-ACTIVE" : "");
}

void printPos() {
  Serial.printf("%s %.6f %.6f", g_pos.valid ? "fix" : "nofix", g_pos.lat, g_pos.lon);
#if GW_HAS_GPS
  if (g_gpsFix) Serial.printf(" (gps, %llus old)", static_cast<unsigned long long>((nowMs() - g_gpsFixMs) / 1000));
  else if (g_pos.valid) Serial.print(" (stored, no gps fix yet)");
  else Serial.printf(" (gps: %u good sentences)", g_nmeaGood);
#else
  if (g_pos.valid) Serial.print(" (stored)");
#endif
  Serial.println();
}

void printStatus() {
  printStats();
  Serial.printf("node %08x %s, %.3f MHz, %d dBm out (%d dBm drive), radio %s, crc err %u, tx err %u\n",
                g_router->id(), GW_BOARD_NAME, g_set.freqMHz, g_radio.outputDbm(), g_radio.driveDbm(),
                g_radioOk ? "ok" : "FAILED", g_radio.crcErrors(), g_radio.txErrors());
  Serial.printf("tables: store %u/%u seen %u/%u neighbors %u/%u sos %u/%u, peer keys %u stored\n",
                static_cast<unsigned>(g_router->storeSize()), static_cast<unsigned>(g_cfg.storeCapacity),
                static_cast<unsigned>(g_router->seenSize()), static_cast<unsigned>(g_cfg.seenCapacity),
                static_cast<unsigned>(g_router->neighborTableSize()), static_cast<unsigned>(kLocCapacity),
                static_cast<unsigned>(g_router->sosCount()), static_cast<unsigned>(g_cfg.sosCapacity),
                static_cast<unsigned>(g_set.peerCount));
  Serial.printf("heap: free %u, min free %u, largest block %u, tables worst case ~%u; uptime %llus\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
                static_cast<unsigned>(g_tableBudget), static_cast<unsigned long long>(nowMs() / 1000));
  Serial.printf("channel key %s, %s%s\n", g_set.havePsk ? "on" : "off",
                g_set.requireEnc ? "plaintext DATA refused" : "plaintext DATA accepted",
                g_set.responder ? ", SOS responder" : "");
  Serial.print("pos ");
  printPos();
}

void printHelp() {
  Serial.print(
      "commands: send <hex-id> <text> | known <hex-id> <lat> <lon> | stats | status | pos\n"
      "          keys | peer <hex-id> <hex-pubkey> | sos <text> | sosstop | sosack <hex-id>\n"
      "settings (saved): setpos <lat> <lon> | setpos clear | psk <64 hex> | psk gen | psk off\n"
      "          power <dBm> | freq <MHz> | responder on|off | requireenc on|off | reboot\n");
}

bool parseOnOff(const std::string& s, bool& v) {
  if (s == "on") v = true;
  else if (s == "off") v = false;
  else return false;
  return true;
}

void handleCommand(std::string line) {
  gw::Router& r = *g_router;
  std::string cmd = word(line);
  if (cmd.empty()) return;
  if (cmd == "send") {
    std::string id = word(line);
    uint32_t dst;
    while (!line.empty() && line[0] == ' ') line.erase(0, 1);
    if (!parseId(id, dst) || line.empty()) {
      Serial.println("usage: send <hex-id> <text>");
      return;
    }
    uint32_t mid = r.send(dst, std::vector<uint8_t>(line.begin(), line.end()), nowMs());
    if (mid) Serial.printf("queued id %08x -> %08x\n", mid, dst);
    else Serial.println("too long to send");
  } else if (cmd == "known") {
    std::string id = word(line), la = word(line), lo = word(line);
    uint32_t nid;
    double lat, lon;
    if (!parseId(id, nid) || !parseDouble(la, lat) || !parseDouble(lo, lon)) {
      Serial.println("usage: known <hex-id> <lat> <lon>");
      return;
    }
    r.addKnownPosition(nid, {lat, lon, true}, nowMs());
    Serial.println("ok");
  } else if (cmd == "stats") {
    printStats();
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "pos") {
    printPos();
  } else if (cmd == "keys") {
    // same format as a `peer` argument, so it can be pasted on the other node
    Serial.printf("%08x %s\n", r.id(), toHex(g_pub.data(), g_pub.size()).c_str());
  } else if (cmd == "peer") {
    std::string id = word(line), hex = word(line);
    uint32_t pid;
    Key32 pub;
    if (!parseId(id, pid) || !fromHex(hex, pub.data(), pub.size()) || !r.addPeerKey(pid, pub.data())) {
      Serial.println("bad peer (want <hex-id> <64 hex chars>)");
    } else if (!gw::mcu::savePeer(g_set, pid, pub)) {
      Serial.printf("ok, but not saved (%u peer keys already stored)\n",
                    static_cast<unsigned>(gw::mcu::kMaxStoredPeers));
    } else {
      Serial.println("ok");
    }
  } else if (cmd == "sos") {
    while (!line.empty() && line[0] == ' ') line.erase(0, 1);
    if (r.startSos(line, 0xFF, 0xFF, nowMs()))
      Serial.printf("SOS on%s, repeating until sosstop\n",
                    line.size() > gw::kSosTextMax ? " (text cut to 60 bytes)" : "");
    else
      Serial.println("SOS rate limited, try again in a minute");
  } else if (cmd == "sosstop") {
    r.stopSos();
    Serial.println("SOS off");
  } else if (cmd == "sosack") {
    uint32_t src;
    if (!parseId(word(line), src)) Serial.println("usage: sosack <hex-id>");
    else if (r.ackSos(src, nowMs())) Serial.printf("acked SOS from %08x\n", src);
    else Serial.printf("no SOS held from %08x\n", src);
  } else if (cmd == "setpos") {
    std::string a = word(line), b = word(line);
    double lat, lon;
    if (a == "clear") {
      g_set.fixedPos = false;
      Serial.println(gw::mcu::saveFixedPos(g_set) ? "stored position cleared (takes effect on reboot)"
                                                   : "save failed");
    } else if (parseDouble(a, lat) && parseDouble(b, lon) && lat >= -90 && lat <= 90 && lon >= -180 &&
               lon <= 180) {
      g_set.fixedPos = true;
      g_set.lat = lat;
      g_set.lon = lon;
      if (!g_gpsFix) {
        g_pos = {lat, lon, true};
        r.setPosition(g_pos, nowMs());
      }
      Serial.println(gw::mcu::saveFixedPos(g_set) ? "ok" : "save failed");
    } else {
      Serial.println("usage: setpos <lat> <lon> | setpos clear");
    }
  } else if (cmd == "psk") {
    std::string a = word(line);
    if (a == "off") {
      g_set.havePsk = false;
      gw::wipe(g_set.psk.data(), g_set.psk.size());
      Serial.println(gw::mcu::savePsk(g_set) ? "channel key removed (takes effect on reboot)" : "save failed");
      return;
    }
    Key32 k;
    if (a == "gen") {
      gw::mcu::secureRandom(k.data(), k.size());
      Serial.printf("new channel key, set it on every node: psk %s\n", toHex(k.data(), k.size()).c_str());
    } else if (!fromHex(a, k.data(), k.size())) {
      Serial.println("usage: psk <64 hex chars> | psk gen | psk off");
      return;
    }
    g_set.psk = k;
    g_set.havePsk = true;
    r.setChannelKey(k.data());
    gw::wipe(k.data(), k.size());
    Serial.println(gw::mcu::savePsk(g_set) ? "channel key on" : "channel key on, but save failed");
  } else if (cmd == "power") {
    double v;
    if (!parseDouble(word(line), v) || v < -9 || v > 40) {
      Serial.printf("usage: power <dBm at the antenna port, max %d>\n", kMaxOutputDbm);
      return;
    }
    g_set.outputDbm = static_cast<int8_t>(g_radio.setOutputDbm(static_cast<int>(v)));
    Serial.printf("%d dBm out (%d dBm drive)%s\n", g_radio.outputDbm(), g_radio.driveDbm(),
                  gw::mcu::saveRadio(g_set) ? "" : ", save failed");
  } else if (cmd == "freq") {
    double v;
    if (!parseDouble(word(line), v) || v < kMinFreqMHz || v > kMaxFreqMHz) {
      Serial.printf("usage: freq <MHz, %.4f..%.4f> (takes effect on reboot)\n", kMinFreqMHz, kMaxFreqMHz);
      return;
    }
    g_set.freqMHz = static_cast<float>(v);
    Serial.println(gw::mcu::saveRadio(g_set) ? "saved, takes effect on reboot" : "save failed");
  } else if (cmd == "responder" || cmd == "requireenc") {
    bool v;
    if (!parseOnOff(word(line), v)) {
      Serial.printf("usage: %s on|off\n", cmd.c_str());
      return;
    }
    (cmd == "responder" ? g_set.responder : g_set.requireEnc) = v;
    Serial.println(gw::mcu::saveFlags(g_set) ? "saved, takes effect on reboot" : "save failed");
  } else if (cmd == "reboot") {
    Serial.println("rebooting");
    Serial.flush();
    ESP.restart();
  } else {
    printHelp();
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (!g_cmdLine.empty()) handleCommand(g_cmdLine);
      g_cmdLine.clear();
    } else if (c == '\b' || c == 0x7F) {
      if (!g_cmdLine.empty()) g_cmdLine.pop_back();
    } else if (g_cmdLine.size() < 200) {
      g_cmdLine += c;
    }
  }
}

#if GW_HAS_GPS
void pollGps(uint64_t now) {
  while (Serial1.available()) {
    char c = static_cast<char>(Serial1.read());
    if (c == '\n') {
      gw::Position p;
      if (gw::parseNmea(g_gpsLine, p)) {
        ++g_nmeaGood;
        g_gpsFix = true;
        g_gpsFixMs = now;
        g_pos = p;
        g_router->setPosition(g_pos, now);
      }
      g_gpsLine.clear();
    } else if (g_gpsLine.size() < 120) {
      g_gpsLine += c;
    }
  }
}
#endif

}  // namespace

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);   // native USB: give the host a moment

  pinMode(kPinVext, OUTPUT);
  digitalWrite(kPinVext, kVextOn ? HIGH : LOW);

  // Keys first (secureRandom borrows the ADC, before anything else uses it).
  bool nvsOk = gw::mcu::loadSettings(g_set);
  if (!nvsOk) Serial.println("NVS unavailable: settings and keys will not persist");
  gw::x25519Public(g_set.secret.data(), g_pub.data());

  g_cfg.nodeId = g_set.nodeId;
  g_cfg.requireEncryption = g_set.requireEnc;
  g_cfg.responder = g_set.responder;
  g_cfg.seenCapacity = kSeenCapacity;
  uint32_t seed[2];
  gw::mcu::secureRandom(reinterpret_cast<uint8_t*>(seed), sizeof seed);
  g_cfg.rngSeed = static_cast<uint64_t>(seed[0]) << 32 | seed[1];
  g_router.reset(new gw::Router(g_cfg, g_radio, g_app));
  g_router->setIdentityKey(g_set.secret.data());
  gw::wipe(g_set.secret.data(), g_set.secret.size());   // the router keeps its own copy
  if (g_set.havePsk) g_router->setChannelKey(g_set.psk.data());
  for (size_t i = 0; i < g_set.peerCount; ++i) g_router->addPeerKey(g_set.peers[i].id, g_set.peers[i].pub.data());

  if (g_set.freqMHz < kMinFreqMHz || g_set.freqMHz > kMaxFreqMHz) g_set.freqMHz = 914.875f;
  int16_t st = g_radio.begin(g_cfg.phy, g_set.freqMHz, g_set.outputDbm);
  g_radioOk = st == 0;
  if (!g_radioOk) Serial.printf("radio init failed: %d\n", st);

  if (g_set.fixedPos) {
    g_pos = {g_set.lat, g_set.lon, true};
    g_router->setPosition(g_pos, nowMs());
  }
#if GW_HAS_GPS
  pinMode(kPinGpsReset, OUTPUT);
  digitalWrite(kPinGpsReset, HIGH);   // out of reset (GPS_RESET_MODE is LOW)
  Serial1.begin(kGpsBaud, SERIAL_8N1, kPinGpsRx, kPinGpsTx);
#endif

  g_tableBudget = worstCaseTableBytes(g_cfg);
  Serial.printf("GhostWave node %08x (%s) on %.3f MHz, %d dBm out\n", g_cfg.nodeId, GW_BOARD_NAME,
                g_set.freqMHz, g_radio.outputDbm());
  Serial.printf("key %s, channel key %s, %u peer key%s%s%s\n", toHex(g_pub.data(), g_pub.size()).c_str(),
                g_set.havePsk ? "on" : "off", static_cast<unsigned>(g_set.peerCount),
                g_set.peerCount == 1 ? "" : "s", g_set.requireEnc ? ", plaintext DATA refused" : "",
                g_set.responder ? ", SOS responder" : "");
  Serial.printf("free heap %u, router tables need up to ~%u%s\n", ESP.getFreeHeap(),
                static_cast<unsigned>(g_tableBudget),
                g_tableBudget + 32 * 1024 > ESP.getFreeHeap() ? "  WARNING: may not fit" : "");
  printHelp();
}

void loop() {
  static uint64_t lastTick = 0;
  static uint8_t buf[256];
  uint64_t now = nowMs();
  size_t len;
  float rssi;
  if (g_radioOk && g_radio.poll(buf, sizeof buf, len, rssi)) g_router->onReceive(buf, len, rssi, now);
#if GW_HAS_GPS
  pollGps(now);
#endif
  pollSerial();
  if (now - lastTick >= 2) {
    lastTick = now;
    if (g_radioOk) g_router->tick(now);
  }
  delay(1);
}
