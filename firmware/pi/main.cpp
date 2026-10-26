// ghostwaved: GhostWave node daemon for a Raspberry Pi with the Backcountry
// Signal HAT (E22P-915M30S / SX1262 + MAX-M10S GPS), or for any Linux/macOS
// host with the UDP virtual radio (pi/radio_udp.cpp).
//
// Usage: sudo ./ghostwaved [--id HEX] [--name NAME] [--freq MHz] [--power dBm]
//                          [--gps /dev/serial0] [--lat L --lon L]  (fixed position, no GPS)
//                          [--nmea FILE]      (NMEA from a file or FIFO, followed like tail -f)
//                          [--key FILE] [--psk FILE] [--peers FILE] [--legacy-x25519]
//                          [--require-enc] [--responder]
//        ./ghostwaved --radio udp [--mcast 239.77.0.1:47700] [--vif ADDR] [--vtx-dbm 24]
//                     [--vgain 2] [--vfade 5] [--vlog] ...   (virtual radio, no hardware)
// stdin commands:  send <hex-id> <text> | known <hex-id> <lat> <lon> | stats | pos
//                  keys | peer [<hex-id>] <hex-pubkey> | ident <hex-id>
//                  sos <text> | sosstop | sosack <hex-id>
//
// Identity (DESIGN §3, §4.5): --key holds an Ed25519 seed as "ed25519:<64 hex>".
// It is created with mode 600 if missing. Default /var/lib/ghostwave/identity.key.
// The node ID is nodeIdOf(public key), so --id is optional and must match it.
// --name (up to 9 bytes) goes out in our IDENT broadcasts.
//
// Older key files hold a bare X25519 secret (64 hex chars or 32 raw bytes). The
// daemon refuses them: move the file aside to make an Ed25519 identity, which
// gives a new node ID, or run with --legacy-x25519. That mode keeps the old key
// and --id, learns peers from IDENT, but cannot send one.
//
// Other key files (DESIGN §9.1):
//   --psk    optional channel key, 64 hex chars or 32 raw bytes. Generate: openssl rand -hex 32
//   --peers  optional "<hex-id> <hex-pubkey>" lines ('#' comments), as printed
//            by the `keys` command on the other node. A bare key is Ed25519 and
//            must hash to the ID; prefix "x25519:" for a legacy node's key.
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

#include "host_radio.h"
#include "ghostwave/crypto.h"
#include "ghostwave/router.h"

uint64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

namespace {

#ifdef GHOSTWAVE_HAVE_E22P
constexpr const char* kDefaultRadio = "e22p";
#else
constexpr const char* kDefaultRadio = "udp";
#endif

struct Options {
  uint32_t id = 0;
  std::string name;
  std::string radio = kDefaultRadio;
  E22pOptions e22p;
  UdpOptions udp;
  std::string gpsDev = "/dev/serial0";
  std::string nmeaFile;
  double lat = 0, lon = 0;
  bool fixedPos = false;
  std::string keyFile = "/var/lib/ghostwave/identity.key";
  std::string pskFile, peersFile;
  bool requireEnc = false, responder = false, legacyX25519 = false;
};

using Key32 = std::array<uint8_t, gw::kKeySize>;

uint32_t machineId() {
  std::ifstream f("/etc/machine-id");
  std::string s;
  std::getline(f, s);
  if (s.empty()) return 0x12345678;
  return gw::crc32c(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// A file or FIFO of NMEA sentences instead of the GPS UART. Reads at EOF
// return 0, so lines appended later are still picked up.
int openNmeaFile(const std::string& path) {
  return open(path.c_str(), O_RDONLY | O_NONBLOCK);
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

// Prefix of an Ed25519 identity file. A bare key is a legacy X25519 secret.
const char kSeedTag[] = "ed25519:";
const char kX25519Tag[] = "x25519:";

bool startsWith(const std::string& s, const char* prefix) {
  return s.compare(0, std::strlen(prefix), prefix) == 0;
}

bool parseId(const std::string& s, uint32_t& id) {
  if (s.empty() || s.size() > 8) return false;
  char* end = nullptr;
  id = static_cast<uint32_t>(std::strtoul(s.c_str(), &end, 16));
  return *end == '\0' && id != 0;
}

// Reads a 32-byte key. With an empty tag: exactly 32 raw bytes, or 64 hex
// chars plus optional whitespace. With a tag: the tag, then 64 hex chars. The
// file buffer is wiped before returning.
bool readKeyFile(const std::string& path, Key32& key, const char* tag = "") {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  char buf[160];
  ssize_t n = read(fd, buf, sizeof buf);
  close(fd);
  bool ok = false;
  if (n == static_cast<ssize_t>(gw::kKeySize) && !*tag) {
    std::memcpy(key.data(), buf, gw::kKeySize);
    ok = true;
  } else if (n > 0) {
    std::string hex;
    for (ssize_t i = 0; i < n; ++i)
      if (!std::isspace(static_cast<unsigned char>(buf[i]))) hex += buf[i];
    if (startsWith(hex, tag)) {
      hex.erase(0, std::strlen(tag));   // in place, so no unwiped copy
      ok = fromHex(hex, key.data(), key.size());
    }
    if (!hex.empty()) gw::wipe(&hex[0], hex.size());
  }
  gw::wipe(buf, sizeof buf);
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

// Makes an Ed25519 seed whose node ID is usable (DESIGN §3). About 1 in 65536
// keys is not, so a few tries are plenty.
bool newSeed(Key32& seed) {
  Key32 pub;
  for (int i = 0; i < 16; ++i) {
    if (!randomBytes(seed.data(), seed.size())) return false;
    gw::ed25519Public(seed.data(), pub.data());
    if (gw::nodeIdUsable(gw::nodeIdOf(pub.data()))) return true;
  }
  return false;
}

// Loads the identity key, creating an Ed25519 seed (mode 600, parent dir 700)
// if the file is missing. Sets `legacy` if the file holds a bare X25519 secret,
// which is an error unless `allowLegacy`.
bool loadIdentity(const std::string& path, bool allowLegacy, Key32& secret, bool& legacy) {
  legacy = false;
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    if (st.st_mode & (S_IRWXG | S_IRWXO))
      std::fprintf(stderr, "warning: %s is readable by others; chmod 600 it\n", path.c_str());
    if (readKeyFile(path, secret, kSeedTag)) {
      if (!allowLegacy) return true;
      gw::wipe(secret.data(), secret.size());
      std::fprintf(stderr, "%s holds an Ed25519 identity; drop --legacy-x25519\n", path.c_str());
      return false;
    }
    if (readKeyFile(path, secret)) {
      legacy = true;
      if (allowLegacy) return true;
      gw::wipe(secret.data(), secret.size());
      std::fprintf(stderr,
                   "%s holds a legacy X25519 key, from before Ed25519 identities (DESIGN §4.5).\n"
                   "Either move it aside so a new Ed25519 identity is made (new node ID; peers\n"
                   "must update their peers files), or run with --legacy-x25519 to keep it\n"
                   "(this node then cannot send IDENT).\n",
                   path.c_str());
      return false;
    }
    std::fprintf(stderr, "bad identity key in %s (want %s and 64 hex chars)\n", path.c_str(), kSeedTag);
    return false;
  }
  if (allowLegacy) {
    std::fprintf(stderr, "--legacy-x25519 needs an existing key file; %s is missing\n", path.c_str());
    return false;
  }
  size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) mkdir(path.substr(0, slash).c_str(), 0700);
  if (!newSeed(secret)) {
    std::fprintf(stderr, "cannot read /dev/urandom\n");
    return false;
  }
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    std::fprintf(stderr, "cannot create %s: %s\n", path.c_str(), std::strerror(errno));
    gw::wipe(secret.data(), secret.size());
    return false;
  }
  fchmod(fd, 0600);   // in case the umask was odd
  std::string hex = kSeedTag + toHex(secret.data(), secret.size()) + "\n";
  bool ok = write(fd, hex.data(), hex.size()) == static_cast<ssize_t>(hex.size());
  ok = (fsync(fd) == 0) && ok;
  close(fd);
  gw::wipe(&hex[0], hex.size());
  if (!ok) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    unlink(path.c_str());
    gw::wipe(secret.data(), secret.size());
    return false;
  }
  std::fprintf(stderr, "generated new Ed25519 identity %s\n", path.c_str());
  return true;
}

// Pins a peer key. `pubHex` is an Ed25519 key, which must hash to the ID, or
// "x25519:" and a legacy node's key. An empty `idHex` takes the ID from the
// Ed25519 key. Fills `err` on failure.
bool addPeer(gw::Router& r, const std::string& idHex, const std::string& pubHex, std::string& err) {
  uint32_t id = 0;
  if (!idHex.empty() && !parseId(idHex, id)) {
    err = "bad id (want up to 8 hex digits)";
    return false;
  }
  uint8_t pub[gw::kKeySize], xPub[gw::kKeySize];
  bool x25519 = startsWith(pubHex, kX25519Tag);
  if (!fromHex(x25519 ? pubHex.substr(std::strlen(kX25519Tag)) : pubHex, pub, sizeof pub)) {
    err = "bad key (want 64 hex chars, or x25519: and 64 hex chars)";
    return false;
  }
  if (x25519) {
    if (!id) {
      err = "an x25519: key needs its node id";
      return false;
    }
    std::memcpy(xPub, pub, sizeof xPub);
  } else {
    uint32_t keyId = gw::nodeIdOf(pub);
    if (!id) id = keyId;
    if (id != keyId || !gw::nodeIdUsable(keyId)) {
      char b[96];
      std::snprintf(b, sizeof b, "Ed25519 key belongs to node %08x, not %08x (legacy key? prefix %s)",
                    keyId, id, kX25519Tag);
      err = b;
      return false;
    }
    gw::ed25519ToX25519(pub, xPub);
  }
  if (r.addPeerKey(id, xPub)) return true;
  err = "key rejected (low-order key, or peer table full)";
  return false;
}

// One "[<hex-id>] <hex-pubkey>" per line; blank lines and '#' comments skipped.
// Returns the number of keys added, or -1 if the file cannot be opened.
int loadPeers(const std::string& path, gw::Router& r) {
  std::ifstream f(path);
  if (!f) return -1;
  std::string line;
  int n = 0, lineNo = 0;
  while (std::getline(f, line)) {
    ++lineNo;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    std::istringstream in(line);
    std::string id, pub, err;
    if (!(in >> id)) continue;
    if (!(in >> pub)) pub.swap(id);
    if (addPeer(r, id, pub, err)) ++n;
    else std::fprintf(stderr, "%s:%d: bad peer line: %s\n", path.c_str(), lineNo, err.c_str());
  }
  return n;
}

class StdoutApp : public gw::AppPort {
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
  void onSos(const gw::SosInfo& s) override {
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
  void onPeerIdent(uint32_t id, const std::string& name, bool firstSeen) override {
    std::printf("IDENT %s %08x \"%s\"\n", firstSeen ? "new peer" : "renamed", id, printable(name).c_str());
    std::fflush(stdout);
  }
  void onKeyChange(uint32_t id, const uint8_t newEdPub[32], const std::string& name) override {
    std::printf("WARNING: KEY CHANGE for %08x \"%s\": a signed IDENT carries a different key.\n"
                "  Node IDs are key hashes, so this is a forged ID collision or a mistake (DESIGN §9.2).\n"
                "  Still using the old key. To accept the new one: peer %08x %s\n",
                id, printable(name).c_str(), id, toHex(newEdPub, gw::kKeySize).c_str());
    std::fflush(stdout);
  }

 private:
  // Names come off the air: keep control bytes out of the terminal.
  static std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (c >= 0x20 && c < 0x7F && c != '"') ? static_cast<char>(c) : '?';
    return out;
  }
};

// Our public key: Ed25519, or X25519 for a --legacy-x25519 node.
struct PubKey {
  Key32 key{};
  bool legacy = false;
};

void handleCommand(const std::string& line, gw::Router& r, HostRadio& radio,
                   const gw::Position& pos, const PubKey& pub) {
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
                "store %zu nb %zu sos held %zu sent %u heard %u ident tx %u rx %u bad %u keychg %u%s\n",
                s.tx, s.rx, s.rxBad, s.rxAuthFail, s.delivered, s.forwarded, s.stored,
                static_cast<unsigned long long>(s.airMs), r.storeSize(), r.neighborCount(nowMs()),
                r.sosCount(), s.sosTx, s.sosHeard, s.identTx, s.identRx, s.identBad, s.keyChanges,
                r.sosActive() ? " SOS-ACTIVE" : "");
    radio.printStats();
  } else if (cmd == "pos") {
    std::printf("%s %.6f %.6f\n", pos.valid ? "fix" : "nofix", pos.lat, pos.lon);
  } else if (cmd == "keys") {
    // same format as a --peers line, so it can be pasted on the other node
    std::printf("%08x %s%s\n", r.id(), pub.legacy ? kX25519Tag : "",
                toHex(pub.key.data(), pub.key.size()).c_str());
  } else if (cmd == "peer") {
    std::string id, hex, err;
    in >> id >> hex;
    if (hex.empty()) hex.swap(id);
    if (hex.empty()) std::printf("usage: peer [<hex-id>] <hex-pubkey>\n");
    else if (addPeer(r, id, hex, err)) std::printf("ok\n");
    else std::printf("bad peer: %s\n", err.c_str());
  } else if (cmd == "ident") {
    std::string id;
    in >> id;
    uint32_t dst;
    if (!parseId(id, dst)) std::printf("usage: ident <hex-id>\n");
    else if (r.requestIdent(dst, nowMs())) std::printf("asked %08x for its IDENT\n", dst);
    else std::printf("cannot ask: no Ed25519 identity (--legacy-x25519)\n");
  } else if (cmd == "sos") {
    std::string text;
    std::getline(in, text);
    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
    if (r.startSos(text, 0xFF, 0xFF, nowMs()))
      std::printf("SOS on%s, repeating until sosstop\n", text.size() > gw::kSosTextMax ? " (text cut to 60 bytes)" : "");
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
                "          keys | peer [<hex-id>] <hex-pubkey> | ident <hex-id>\n"
                "          sos <text> | sosstop | sosack <hex-id>\n");
  }
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--id") {
      if (!parseId(next(), o.id)) { std::fprintf(stderr, "bad --id (want up to 8 hex digits)\n"); return 2; }
    }
    else if (a == "--name") o.name = next();
    else if (a == "--radio") o.radio = next();
    else if (a == "--freq") o.e22p.freqMHz = std::atof(next());
    else if (a == "--power") o.e22p.powerDbm = static_cast<int8_t>(std::atoi(next()));
    else if (a == "--gps") o.gpsDev = next();
    else if (a == "--nmea") o.nmeaFile = next();
    else if (a == "--mcast") {
      std::string g = next();
      size_t colon = g.rfind(':');
      if (colon != std::string::npos) {
        o.udp.port = std::atoi(g.c_str() + colon + 1);
        g.erase(colon);
      }
      o.udp.group = g;
    }
    else if (a == "--vif") o.udp.iface = next();
    else if (a == "--vtx-dbm") o.udp.txDbm = static_cast<float>(std::atof(next()));
    else if (a == "--vgain") o.udp.rxGainDb = static_cast<float>(std::atof(next()));
    else if (a == "--vfade") o.udp.fadeSigmaDb = std::atof(next());
    else if (a == "--vlog") o.udp.log = true;
    else if (a == "--lat") { o.lat = std::atof(next()); o.fixedPos = true; }
    else if (a == "--lon") { o.lon = std::atof(next()); o.fixedPos = true; }
    else if (a == "--key") o.keyFile = next();
    else if (a == "--psk") o.pskFile = next();
    else if (a == "--peers") o.peersFile = next();
    else if (a == "--require-enc") o.requireEnc = true;
    else if (a == "--responder") o.responder = true;
    else if (a == "--legacy-x25519") o.legacyX25519 = true;
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  if (o.name.size() > gw::kIdentNameMax) {
    std::fprintf(stderr, "--name is %zu bytes; at most %zu fit in an IDENT\n", o.name.size(), gw::kIdentNameMax);
    return 2;
  }
  if (o.legacyX25519 && !o.name.empty()) {
    std::fprintf(stderr, "--name needs an Ed25519 identity; a --legacy-x25519 node sends no IDENT\n");
    return 2;
  }
#ifdef GHOSTWAVE_HAVE_E22P
  bool radioOk = o.radio == "e22p" || o.radio == "udp";
#else
  bool radioOk = o.radio == "udp";
#endif
  if (!radioOk) {
    std::fprintf(stderr, "unknown or unbuilt radio '%s' (this build: %s)\n", o.radio.c_str(),
#ifdef GHOSTWAVE_HAVE_E22P
                 "e22p, udp");
#else
                 "udp; build with -DGHOSTWAVE_BUILD_PI=ON for e22p");
#endif
    return 2;
  }

  // Keys first: no point bringing the radio up if they are unusable. The
  // Router does not touch the radio until onReceive / tick.
  Key32 secret{}, psk{};
  PubKey pub;
  if (!loadIdentity(o.keyFile, o.legacyX25519, secret, pub.legacy)) return 1;
  if (pub.legacy) {
    gw::x25519Public(secret.data(), pub.key.data());
    if (!o.id) o.id = machineId();
  } else {
    gw::ed25519Public(secret.data(), pub.key.data());
    uint32_t keyId = gw::nodeIdOf(pub.key.data());
    if (!gw::nodeIdUsable(keyId)) {
      std::fprintf(stderr, "the key in %s gives unusable node ID %08x; move it aside to make a new one\n",
                   o.keyFile.c_str(), keyId);
      return 1;
    }
    if (o.id && o.id != keyId) {
      std::fprintf(stderr, "--id %08x does not match the identity key in %s, which is node %08x\n",
                   o.id, o.keyFile.c_str(), keyId);
      return 1;
    }
    o.id = keyId;
  }
  bool havePsk = false;
  if (!o.pskFile.empty()) {
    if (!readKeyFile(o.pskFile, psk)) {
      std::fprintf(stderr, "bad channel key in %s (want 64 hex chars or 32 raw bytes)\n", o.pskFile.c_str());
      return 1;
    }
    havePsk = true;
  }

  gw::Config cfg;
  cfg.nodeId = o.id;
  cfg.requireEncryption = o.requireEnc;
  cfg.responder = o.responder;
  std::unique_ptr<HostRadio> radio;
#ifdef GHOSTWAVE_HAVE_E22P
  if (o.radio == "e22p") radio = makeE22pRadio(o.e22p, cfg.phy);
#endif
  if (o.radio == "udp") radio = makeUdpRadio(o.udp, cfg.phy, o.id);
  if (!radio) {
    gw::wipe(secret.data(), secret.size());
    gw::wipe(psk.data(), psk.size());
    return 1;
  }
  StdoutApp app;
  gw::Router router(cfg, *radio, app);
  if (pub.legacy) {
    router.setIdentityKey(secret.data());
  } else if (!router.setSigningKey(secret.data(), o.name)) {
    std::fprintf(stderr, "setSigningKey failed\n");   // ID and name were checked above
    gw::wipe(secret.data(), secret.size());
    gw::wipe(psk.data(), psk.size());
    return 1;
  }
  gw::wipe(secret.data(), secret.size());
  if (havePsk) router.setChannelKey(psk.data());
  gw::wipe(psk.data(), psk.size());
  int peers = 0;
  if (!o.peersFile.empty()) {
    peers = loadPeers(o.peersFile, router);
    if (peers < 0) {
      std::fprintf(stderr, "cannot open peers file %s\n", o.peersFile.c_str());
      return 1;
    }
  }

  gw::Position pos;
  if (o.fixedPos) {
    pos = {o.lat, o.lon, true};
    router.setPosition(pos, nowMs());
    radio->setPosition(pos);
  }
  const std::string& gpsSrc = o.nmeaFile.empty() ? o.gpsDev : o.nmeaFile;
  int gps = o.fixedPos ? -1 : o.nmeaFile.empty() ? openGps(o.gpsDev) : openNmeaFile(o.nmeaFile);
  if (!o.fixedPos && gps < 0) std::fprintf(stderr, "warning: cannot open %s, no position\n", gpsSrc.c_str());

  if (o.radio == "udp")
    std::printf("ghostwaved node %08x on virtual radio %s:%d, %.0f dBm EIRP\n", o.id,
                o.udp.group.c_str(), o.udp.port, o.udp.txDbm);
  else
    std::printf("ghostwaved node %08x on %.3f MHz, %d dBm drive\n", o.id, o.e22p.freqMHz,
                o.e22p.powerDbm);
  if (pub.legacy)
    std::printf("key x25519:%s (legacy, no IDENT)", toHex(pub.key.data(), pub.key.size()).c_str());
  else
    std::printf("key ed25519 %s, name \"%s\"", toHex(pub.key.data(), pub.key.size()).c_str(), o.name.c_str());
  std::printf(", channel key %s, %d peer key%s%s%s\n", havePsk ? "on" : "off", peers, peers == 1 ? "" : "s",
              o.requireEnc ? ", plaintext DATA refused" : "", o.responder ? ", SOS responder" : "");
  std::fflush(stdout);

  std::string gpsLine, cmdLine;
  uint8_t buf[256];
  for (;;) {
    uint64_t now = nowMs();
    float rssi = 0;
    if (size_t len = radio->receive(buf, sizeof buf, rssi)) router.onReceive(buf, len, rssi, now);
    if (gps >= 0) {
      char c[128];
      ssize_t n = read(gps, c, sizeof c);
      for (ssize_t i = 0; i < n; ++i) {
        if (c[i] == '\n') {
          gw::Position p;
          if (gw::parseNmea(gpsLine, p)) {
            pos = p;
            router.setPosition(pos, now);
            radio->setPosition(pos);
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
          handleCommand(cmdLine, router, *radio, pos, pub);
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
