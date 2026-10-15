// GhostWave node firmware for ESP32-S3 + SX1262 dev boards (Heltec Wireless
// Tracker V2, Heltec V3). Same core and the same line commands as ghostwaved
// (firmware/pi), over USB serial at 115200 baud. Type `help`.
//
// Identity: an Ed25519 seed in NVS; the node ID is derived from its public key
// (DESIGN §3) and peers learn our key from signed IDENT broadcasts (§4.5).
// The seed, the optional channel key, pinned peer keys and settings live in NVS
// (Preferences namespace "ghostwave"). NVS is not encrypted: anyone holding
// the board can read the keys out of flash.
#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include "board.h"
#include "bootloader_random.h"
#include "ghostwave/crypto.h"
#include "ghostwave/router.h"

namespace {

// "Trail" preset (DESIGN §2): SF9 / BW125 / CR4/5 / 16-symbol preamble.
// Frequency and sync word match ghostwaved so a Pi node and a dev board hear
// each other: 914.875 MHz sits inside the US 915 band, clear of the
// Meshtastic/MeshCore defaults, and 0x24 keeps other meshes' frames out.
constexpr float kDefaultFreqMHz = 914.875f;
constexpr uint8_t kSyncWord = 0x24;
constexpr size_t kSeenCapacity = 768;   // see README "Memory"
constexpr size_t kMaxPeers = 16;        // persisted peer keys
constexpr size_t kLineMax = 300;        // CLI line
constexpr size_t kNmeaMax = 120;
constexpr uint32_t kLowHeapBytes = 24 * 1024;

using Key32 = std::array<uint8_t, gw::kKeySize>;

struct PeerRec {
  uint32_t id;
  uint8_t pub[gw::kKeySize];
};

struct Settings {
  uint32_t id = 0;
  Key32 seed{};       // Ed25519 identity seed
  std::string name;   // IDENT name, <= kIdentNameMax bytes
  bool havePsk = false;
  Key32 psk{};
  bool fixedPos = false;
  double lat = 0, lon = 0;
  int8_t outputDbm = kDefaultOutputDbm;
  float freqMHz = kDefaultFreqMHz;
  bool responder = false, requireEnc = false;
  std::vector<PeerRec> peers;
};

Preferences g_prefs;
Settings g_set;
Key32 g_pub{};     // X25519 public key (what `keys` prints)
Key32 g_edPub{};   // Ed25519 identity key

SPIClass g_spi(FSPI);
SX1262 g_radio = new Module(kPinLoraCs, kPinLoraDio1, kPinLoraRst, kPinLoraBusy, g_spi);
volatile bool g_rxFlag = false;
volatile bool g_radioOp = false;   // CAD or TX in progress: DIO1 is not "packet received"
int8_t g_driveDbm = 0;

gw::Router* g_router = nullptr;
gw::Position g_pos;
const char* g_posSrc = "none";
#if GW_HAS_GPS
uint32_t g_nmeaOk = 0, g_fixes = 0;
#endif
bool g_lowHeapWarned = false;

uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

// ------------------------------------------------------------------ helpers --

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
  return *end == '\0' && id != 0 && id != gw::kBroadcast;
}

// Splits off the next space-separated word; `rest` keeps the remainder.
std::string word(std::string& rest) {
  size_t a = rest.find_first_not_of(' ');
  if (a == std::string::npos) {
    rest.clear();
    return "";
  }
  size_t b = rest.find(' ', a);
  std::string w = rest.substr(a, b == std::string::npos ? std::string::npos : b - a);
  rest = b == std::string::npos ? "" : rest.substr(b + 1);
  return w;
}

// Key material from the hardware RNG. With Wi-Fi/BT off the RNG is only a true
// RNG while the SAR ADC entropy source is enabled.
void secureRandom(uint8_t* out, size_t n) {
  bootloader_random_enable();
  esp_fill_random(out, n);
  bootloader_random_disable();
}

// --------------------------------------------------------------- settings --

void savePeers() {
  g_prefs.putBytes("peers", g_set.peers.data(), g_set.peers.size() * sizeof(PeerRec));
}

void loadSettings() {
  g_prefs.begin("ghostwave", false);
  if (g_prefs.getBytes("seed", g_set.seed.data(), g_set.seed.size()) != g_set.seed.size()) {
    // the node ID is a hash of the key: redraw the rare key whose ID is unusable
    do {
      secureRandom(g_set.seed.data(), g_set.seed.size());
      gw::ed25519Public(g_set.seed.data(), g_edPub.data());
    } while (!gw::nodeIdUsable(gw::nodeIdOf(g_edPub.data())));
    g_prefs.putBytes("seed", g_set.seed.data(), g_set.seed.size());
    Serial.println("generated new identity key");
  }
  gw::ed25519Public(g_set.seed.data(), g_edPub.data());
  gw::ed25519ToX25519(g_edPub.data(), g_pub.data());
  g_set.id = gw::nodeIdOf(g_edPub.data());
  g_set.name = g_prefs.getString("name", "").c_str();
  g_set.havePsk = g_prefs.getBytes("psk", g_set.psk.data(), g_set.psk.size()) == g_set.psk.size();
  g_set.fixedPos = g_prefs.getBool("fix", false);
  g_set.lat = g_prefs.getDouble("lat", 0);
  g_set.lon = g_prefs.getDouble("lon", 0);
  g_set.outputDbm = g_prefs.getChar("pwr", kDefaultOutputDbm);
  g_set.freqMHz = g_prefs.getFloat("freq", kDefaultFreqMHz);
  g_set.responder = g_prefs.getBool("resp", false);
  g_set.requireEnc = g_prefs.getBool("reqenc", false);
  size_t n = g_prefs.getBytesLength("peers") / sizeof(PeerRec);
  if (n > kMaxPeers) n = kMaxPeers;
  g_set.peers.resize(n);
  if (n) g_prefs.getBytes("peers", g_set.peers.data(), n * sizeof(PeerRec));
}

// ------------------------------------------------------------------ radio --

// Highest SX1262 drive whose output (drive + FEM gain) stays within `out` dBm.
int8_t driveFor(int out) {
  if (out > kMaxOutputDbm) out = kMaxOutputDbm;
#if GW_HAS_KCT8103L
  if (out - kPaGainDb[0] < 0) return static_cast<int8_t>(out - kPaGainDb[0] < -9 ? -9 : out - kPaGainDb[0]);
  int best = 0;
  for (int d = 0; d < 22; ++d)
    if (d + kPaGainDb[d] <= out) best = d;
  return static_cast<int8_t>(best);
#else
  return static_cast<int8_t>(out < -9 ? -9 : out);
#endif
}

int outputFor(int8_t drive) {
#if GW_HAS_KCT8103L
  return drive + kPaGainDb[drive < 0 ? 0 : drive];
#else
  return drive;
#endif
}

void IRAM_ATTR onDio1() {
  if (!g_radioOp) g_rxFlag = true;
}

void femTx(bool tx) {
#if GW_HAS_KCT8103L
  digitalWrite(kPinFemCtx, tx ? HIGH : LOW);
#else
  (void)tx;
#endif
}

class McuRadio : public gw::RadioPort {
 public:
  bool channelBusy() override {
    if (g_rxFlag) return true;   // a packet is waiting
    // a header already decoded: a frame is arriving, and CAD would abort it
    if (g_radio.checkIrq(RADIOLIB_IRQ_HEADER_VALID) == 1) return true;
    g_radioOp = true;
    int16_t r = g_radio.scanChannel();   // CAD
    g_radioOp = false;
    g_radio.startReceive();
    return r == RADIOLIB_LORA_DETECTED;
  }
  bool transmit(const uint8_t* buf, size_t len) override {
    femTx(true);
    g_radioOp = true;
    int16_t st = g_radio.transmit(const_cast<uint8_t*>(buf), len);
    g_radioOp = false;
    femTx(false);
    g_radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) Serial.printf("tx failed: %d\n", st);
    return st == RADIOLIB_ERR_NONE;
  }
};

bool radioBegin() {
#if GW_HAS_KCT8103L
  // FEM bring-up order from LoRaFEMInterface::init (USE_KCT8103L_PA)
  pinMode(kPinFemPower, OUTPUT);
  digitalWrite(kPinFemPower, HIGH);
  delay(1);
  pinMode(kPinFemCsd, OUTPUT);
  digitalWrite(kPinFemCsd, HIGH);
  pinMode(kPinFemCtx, OUTPUT);
  digitalWrite(kPinFemCtx, LOW);   // RX through the LNA
#endif
  g_spi.begin(kPinLoraSck, kPinLoraMiso, kPinLoraMosi, kPinLoraCs);
  g_driveDbm = driveFor(g_set.outputDbm);
  int16_t st = g_radio.begin(g_set.freqMHz, 125.0, 9, 5, kSyncWord, g_driveDbm, 16, kTcxoVolts, false);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("radio init failed: %d\n", st);
    return false;
  }
  g_radio.setDio2AsRfSwitch(true);
  g_radio.setCRC(2);
  g_radio.setCurrentLimit(140);
  g_radio.setPacketReceivedAction(onDio1);
  g_radio.startReceive();
  return true;
}

// -------------------------------------------------------------------- app --

class SerialApp : public gw::AppPort {
 public:
  void onMessage(uint32_t src, uint32_t msgId, const std::vector<uint8_t>& p) override {
    Serial.printf("MSG from %08lx id %08lx: %.*s\n", (unsigned long)src, (unsigned long)msgId,
                  static_cast<int>(p.size()), reinterpret_cast<const char*>(p.data()));
  }
  void onDelivered(uint32_t dst, uint32_t msgId) override {
    Serial.printf("DELIVERED to %08lx id %08lx\n", (unsigned long)dst, (unsigned long)msgId);
  }
  void onSos(const gw::SosInfo& s) override {
    Serial.printf("SOS from %08lx seq %u", (unsigned long)s.src, s.seq);
    if (s.pos.valid) Serial.printf(" at %.6f %.6f", s.pos.lat, s.pos.lon);
    else Serial.print(" no fix");
    if (s.accuracy != 0xFF) Serial.printf(" +-%um", s.accuracy * 4u);
    if (s.battery != 0xFF) Serial.printf(" batt %u%%", s.battery);
    Serial.printf(": %s\n  (ack with: sosack %08lx)\n", s.text.c_str(), (unsigned long)s.src);
  }
  void onSosAck(uint32_t responder, uint16_t seq) override {
    Serial.printf("SOSACK from %08lx for seq %u\n", (unsigned long)responder, seq);
  }
  void onPeerIdent(uint32_t id, const std::string& name, bool firstSeen) override {
    Serial.printf("IDENT %s %08lx%s%s\n", firstSeen ? "learned key of" : "name change", (unsigned long)id,
                  name.empty() ? "" : " ", name.c_str());
  }
  void onKeyChange(uint32_t id, const uint8_t newEdPub[32], const std::string& name) override {
    uint8_t x[gw::kKeySize];
    gw::ed25519ToX25519(newEdPub, x);
    Serial.printf("WARNING: %08lx%s%s sent a validly signed IDENT with a DIFFERENT key. The old key stays in use.\n"
                  "  This means a forged ID collision unless you know why. To accept the new key:\n"
                  "  peer %08lx %s\n",
                  (unsigned long)id, name.empty() ? "" : " ", name.c_str(), (unsigned long)id,
                  toHex(x, sizeof x).c_str());
  }
};

McuRadio g_radioPort;
SerialApp g_app;

// -------------------------------------------------------------------- CLI --

void printStats() {
  auto& s = g_router->stats();
  Serial.printf("tx %lu rx %lu bad %lu authfail %lu delivered %lu forwarded %lu stored %lu air %llums "
                "store %u nb %u sos held %u sent %lu heard %lu%s\n",
                (unsigned long)s.tx, (unsigned long)s.rx, (unsigned long)s.rxBad, (unsigned long)s.rxAuthFail,
                (unsigned long)s.delivered, (unsigned long)s.forwarded, (unsigned long)s.stored,
                (unsigned long long)s.airMs, (unsigned)g_router->storeSize(),
                (unsigned)g_router->neighborCount(nowMs()), (unsigned)g_router->sosCount(),
                (unsigned long)s.sosTx, (unsigned long)s.sosHeard, g_router->sosActive() ? " SOS-ACTIVE" : "");
}

void printStatus() {
  gw::Config def;
  Serial.printf("node %08lx%s%s  board %s  up %llus\n", (unsigned long)g_set.id, g_set.name.empty() ? "" : " ",
                g_set.name.c_str(), GW_BOARD_NAME, (unsigned long long)(nowMs() / 1000));
  Serial.printf("radio %.3f MHz SF9 BW125 CR4/5 pre16  out %d dBm (drive %d)\n", g_set.freqMHz,
                outputFor(g_driveDbm), g_driveDbm);
  Serial.printf("pos %s %.6f %.6f (%s)", g_pos.valid ? "fix" : "nofix", g_pos.lat, g_pos.lon, g_posSrc);
#if GW_HAS_GPS
  Serial.printf("  gps sentences %lu fixes %lu", (unsigned long)g_nmeaOk, (unsigned long)g_fixes);
#endif
  Serial.printf("\nkeys: channel %s, %u peer%s%s%s\n", g_set.havePsk ? "on" : "off",
                (unsigned)g_set.peers.size(), g_set.peers.size() == 1 ? "" : "s",
                g_set.requireEnc ? ", plaintext DATA refused" : "", g_set.responder ? ", SOS responder" : "");
  printStats();
  auto& st = g_router->stats();
  Serial.printf("ident tx %lu rx %lu bad %lu key-changes %lu\n", (unsigned long)st.identTx,
                (unsigned long)st.identRx, (unsigned long)st.identBad, (unsigned long)st.keyChanges);
  Serial.printf("tables: store %u/%u  seen %u/%u  neighbors %u/512  sos %u/%u\n",
                (unsigned)g_router->storeSize(), (unsigned)def.storeCapacity, (unsigned)g_router->seenSize(),
                (unsigned)kSeenCapacity, (unsigned)g_router->neighborTableSize(), (unsigned)g_router->sosCount(),
                (unsigned)def.sosCapacity);
  Serial.printf("heap: free %lu  min-ever %lu  largest block %lu bytes\n", (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void printHelp() {
  Serial.print(
      "messages: send <hex-id> <text> | known <hex-id> <lat> <lon>\n"
      "sos:      sos <text> | sosstop | sosack <hex-id>\n"
      "keys:     keys | ident <hex-id> | peer <hex-id> <hex-pubkey> | peers | psk <64 hex>|off\n"
      "info:     status | stats | pos\n"
      "settings: setpos <lat> <lon>|clear | power <dBm> | freq <MHz> | name <text>\n"
      "          responder on|off | requireenc on|off | reboot | wipe confirm\n");
}

bool onOff(const std::string& s, bool& out) {
  if (s == "on") out = true;
  else if (s == "off") out = false;
  else return false;
  return true;
}

bool addPeer(const std::string& idHex, const std::string& pubHex) {
  PeerRec rec{};
  if (!parseId(idHex, rec.id) || !fromHex(pubHex, rec.pub, sizeof rec.pub)) return false;
  if (!g_router->addPeerKey(rec.id, rec.pub)) return false;
  for (auto& p : g_set.peers) {
    if (p.id == rec.id) {
      p = rec;
      savePeers();
      return true;
    }
  }
  if (g_set.peers.size() >= kMaxPeers) {
    Serial.printf("(peer table full: %u kept in NVS; this one lasts until reboot)\n", (unsigned)kMaxPeers);
    return true;
  }
  g_set.peers.push_back(rec);
  savePeers();
  return true;
}

void handleCommand(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  std::string rest = line;
  std::string cmd = word(rest);
  gw::Router& r = *g_router;
  if (cmd == "send") {
    std::string id = word(rest);
    uint32_t dst;
    if (!parseId(id, dst)) {
      Serial.println("usage: send <hex-id> <text>");
      return;
    }
    uint32_t mid = r.send(dst, std::vector<uint8_t>(rest.begin(), rest.end()), nowMs());
    if (mid) Serial.printf("queued id %08lx -> %08lx\n", (unsigned long)mid, (unsigned long)dst);
    else Serial.println("too long to send (keep it under ~80 bytes)");
  } else if (cmd == "known") {
    std::string id = word(rest), la = word(rest), lo = word(rest);
    uint32_t who;
    if (!parseId(id, who) || la.empty() || lo.empty()) {
      Serial.println("usage: known <hex-id> <lat> <lon>");
      return;
    }
    r.addKnownPosition(who, {std::atof(la.c_str()), std::atof(lo.c_str()), true}, nowMs());
    Serial.println("ok");
  } else if (cmd == "stats") {
    printStats();
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "pos") {
    Serial.printf("%s %.6f %.6f\n", g_pos.valid ? "fix" : "nofix", g_pos.lat, g_pos.lon);
  } else if (cmd == "keys") {
    // same format as a ghostwaved --peers line, so it can be pasted on the other node
    Serial.printf("%08lx %s\n", (unsigned long)r.id(), toHex(g_pub.data(), g_pub.size()).c_str());
    Serial.printf("(ed25519 identity %s)\n", toHex(g_edPub.data(), g_edPub.size()).c_str());
  } else if (cmd == "ident") {
    uint32_t who;
    if (!parseId(word(rest), who)) Serial.println("usage: ident <hex-id>");
    else Serial.println(r.requestIdent(who, nowMs()) ? "IDENT requested" : "cannot request IDENT now");
  } else if (cmd == "name") {
    if (rest.size() > gw::kIdentNameMax) {
      Serial.printf("name is at most %u bytes\n", (unsigned)gw::kIdentNameMax);
      return;
    }
    g_prefs.putString("name", rest.c_str());
    Serial.println("saved; reboot to apply");
  } else if (cmd == "peer") {
    std::string id = word(rest), hex = word(rest);
    Serial.println(addPeer(id, hex) ? "ok" : "bad peer (want <hex-id> <64 hex chars>)");
  } else if (cmd == "peers") {
    for (auto& p : g_set.peers)
      Serial.printf("%08lx %s\n", (unsigned long)p.id, toHex(p.pub, sizeof p.pub).c_str());
    if (g_set.peers.empty()) Serial.println("(none)");
  } else if (cmd == "psk") {
    std::string arg = word(rest);
    if (arg == "off") {
      g_prefs.remove("psk");
      Serial.println("channel key removed; reboot to stop using it");
    } else {
      Key32 k{};
      if (!fromHex(arg, k.data(), k.size())) {
        Serial.println("usage: psk <64 hex chars> | psk off");
        return;
      }
      g_prefs.putBytes("psk", k.data(), k.size());
      r.setChannelKey(k.data());
      g_set.havePsk = true;
      gw::wipe(k.data(), k.size());
      Serial.println("channel key set");
    }
    gw::wipe(&arg[0], arg.size());
  } else if (cmd == "sos") {
    if (r.startSos(rest, 0xFF, 0xFF, nowMs()))
      Serial.printf("SOS on%s, repeating until sosstop\n",
                    rest.size() > gw::kSosTextMax ? " (text cut to 60 bytes)" : "");
    else
      Serial.println("SOS rate limited, try again in a minute");
  } else if (cmd == "sosstop") {
    r.stopSos();
    Serial.println("SOS off");
  } else if (cmd == "sosack") {
    uint32_t src;
    if (!parseId(word(rest), src)) Serial.println("usage: sosack <hex-id>");
    else if (r.ackSos(src, nowMs())) Serial.printf("acked SOS from %08lx\n", (unsigned long)src);
    else Serial.printf("no SOS held from %08lx\n", (unsigned long)src);
  } else if (cmd == "setpos") {
    std::string la = word(rest), lo = word(rest);
    if (la == "clear") {
      g_prefs.putBool("fix", false);
      g_set.fixedPos = false;
      Serial.println("stored position cleared (takes effect on reboot)");
    } else if (!la.empty() && !lo.empty()) {
      double lat = std::atof(la.c_str()), lon = std::atof(lo.c_str());
      if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        Serial.println("lat must be -90..90, lon -180..180");
        return;
      }
      g_set.fixedPos = true;
      g_set.lat = lat;
      g_set.lon = lon;
      g_prefs.putBool("fix", true);
      g_prefs.putDouble("lat", lat);
      g_prefs.putDouble("lon", lon);
      if (!g_pos.valid || g_posSrc[0] != 'g') {
        g_pos = {lat, lon, true};
        g_posSrc = "stored";
        r.setPosition(g_pos, nowMs());
      }
      Serial.println("ok");
    } else {
      Serial.println("usage: setpos <lat> <lon> | setpos clear");
    }
  } else if (cmd == "power") {
    std::string v = word(rest);
    if (v.empty()) {
      Serial.printf("out %d dBm (drive %d), max %d\n", outputFor(g_driveDbm), g_driveDbm, kMaxOutputDbm);
      return;
    }
    int dbm = std::atoi(v.c_str());
    if (dbm > kMaxOutputDbm) dbm = kMaxOutputDbm;
    int8_t drive = driveFor(dbm);
    if (g_radio.setOutputPower(drive) != RADIOLIB_ERR_NONE) {
      Serial.println("radio refused that power");
      return;
    }
    g_radio.startReceive();
    g_driveDbm = drive;
    g_set.outputDbm = static_cast<int8_t>(dbm);
    g_prefs.putChar("pwr", g_set.outputDbm);
    Serial.printf("out %d dBm (drive %d)\n", outputFor(drive), drive);
  } else if (cmd == "freq") {
    float f = std::atof(word(rest).c_str());
    if (f < 902.0f || f > 928.0f) {
      Serial.println("usage: freq <902..928 MHz> (US build)");
      return;
    }
    if (g_radio.setFrequency(f) != RADIOLIB_ERR_NONE) {
      Serial.println("radio refused that frequency");
      return;
    }
    g_radio.startReceive();
    g_set.freqMHz = f;
    g_prefs.putFloat("freq", f);
    Serial.printf("%.3f MHz\n", f);
  } else if (cmd == "responder" || cmd == "requireenc") {
    bool v;
    if (!onOff(word(rest), v)) {
      Serial.printf("usage: %s on|off\n", cmd.c_str());
      return;
    }
    g_prefs.putBool(cmd == "responder" ? "resp" : "reqenc", v);
    Serial.println("saved; reboot to apply");
  } else if (cmd == "reboot") {
    Serial.flush();
    ESP.restart();
  } else if (cmd == "wipe") {
    if (word(rest) != "confirm") {
      Serial.println("erases the identity key (and so the node ID), channel key, peers and settings: wipe confirm");
      return;
    }
    g_prefs.clear();
    Serial.println("wiped, rebooting");
    Serial.flush();
    ESP.restart();
  } else if (!cmd.empty()) {
    printHelp();
  }
}

std::string g_cmdLine, g_nmeaLine;

void pollSerial() {
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (!g_cmdLine.empty()) handleCommand(g_cmdLine);
      g_cmdLine.clear();
    } else if (g_cmdLine.size() < kLineMax) {
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
      if (gw::parseNmea(g_nmeaLine, p)) {
        ++g_fixes;
        g_pos = p;
        g_posSrc = "gps";
        g_router->setPosition(p, now);
      }
      if (!g_nmeaLine.empty() && g_nmeaLine[0] == '$') ++g_nmeaOk;
      g_nmeaLine.clear();
    } else if (g_nmeaLine.size() < kNmeaMax) {
      g_nmeaLine += c;
    }
  }
}

void gpsBegin() {
  pinMode(kPinGpsReset, OUTPUT);
  digitalWrite(kPinGpsReset, HIGH);   // out of reset (GPS_RESET_MODE LOW)
  Serial1.begin(kGpsBaud, SERIAL_8N1, kPinGpsRx, kPinGpsTx);
}
#endif

}  // namespace

// Ed25519 sign/verify (IDENT) runs in loop(): double the default 8 KB stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);   // native USB: give the host a moment
  pinMode(kPinVext, OUTPUT);
  digitalWrite(kPinVext, kVextOn ? HIGH : LOW);

  loadSettings();
  gw::Config cfg;
  cfg.nodeId = g_set.id;
  cfg.requireEncryption = g_set.requireEnc;
  cfg.responder = g_set.responder;
  cfg.seenCapacity = kSeenCapacity;
  uint64_t seed;
  esp_fill_random(&seed, sizeof seed);
  cfg.rngSeed = seed;
  g_router = new gw::Router(cfg, g_radioPort, g_app);
  bool identOk = g_router->setSigningKey(g_set.seed.data(), g_set.name);
  gw::wipe(g_set.seed.data(), g_set.seed.size());
  if (!identOk) Serial.println("identity key rejected (name too long?); IDENT disabled");
  if (g_set.havePsk) g_router->setChannelKey(g_set.psk.data());
  gw::wipe(g_set.psk.data(), g_set.psk.size());
  for (auto& p : g_set.peers)
    if (!g_router->addPeerKey(p.id, p.pub)) Serial.printf("stored peer %08lx rejected\n", (unsigned long)p.id);

  if (g_set.fixedPos) {
    g_pos = {g_set.lat, g_set.lon, true};
    g_posSrc = "stored";
    g_router->setPosition(g_pos, nowMs());
  }
#if GW_HAS_GPS
  gpsBegin();
#endif

  bool radioOk = radioBegin();
  Serial.printf("ghostwave %s node %08lx%s%s on %.3f MHz, %d dBm out\n", GW_BOARD_NAME, (unsigned long)g_set.id,
                g_set.name.empty() ? "" : " ", g_set.name.c_str(), g_set.freqMHz, outputFor(g_driveDbm));
  Serial.printf("key %s, channel key %s, %u peer key%s%s%s\n", toHex(g_pub.data(), g_pub.size()).c_str(),
                g_set.havePsk ? "on" : "off", (unsigned)g_set.peers.size(), g_set.peers.size() == 1 ? "" : "s",
                g_set.requireEnc ? ", plaintext DATA refused" : "", g_set.responder ? ", SOS responder" : "");
  if (!radioOk) {
    Serial.println("radio not running; check the board env matches the hardware. Halted.");
    for (;;) delay(1000);
  }
  Serial.println("type `help`");
}

void loop() {
  static uint8_t buf[256];
  static uint64_t heapCheckAt = 0;
  uint64_t now = nowMs();
  if (g_rxFlag) {
    g_rxFlag = false;
    size_t len = g_radio.getPacketLength();
    if (len > 0 && len <= sizeof buf && g_radio.readData(buf, len) == RADIOLIB_ERR_NONE)
      g_router->onReceive(buf, len, g_radio.getRSSI(), now);
    g_radio.startReceive();
  }
#if GW_HAS_GPS
  pollGps(now);
#endif
  pollSerial();
  g_router->tick(now);
  if (now >= heapCheckAt) {
    heapCheckAt = now + 10000;
    if (!g_lowHeapWarned && ESP.getMinFreeHeap() < kLowHeapBytes) {
      g_lowHeapWarned = true;
      Serial.printf("warning: free heap fell to %lu bytes\n", (unsigned long)ESP.getMinFreeHeap());
    }
  }
  delay(2);
}
