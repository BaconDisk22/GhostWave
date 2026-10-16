// Host tests for the GhostWave core. No framework: each CHECK prints and counts.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "ghostwave/crypto.h"
#include "ghostwave/packet.h"
#include "ghostwave/router.h"
#include "ghostwave/util.h"
#include "vchannel.h"

using namespace gw;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (cond) {                                                         \
      ++g_pass;                                                         \
    } else {                                                            \
      ++g_fail;                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                   \
  } while (0)

// ---------------------------------------------------------------- packets --

static Packet fullPacket() {
  Packet p;
  p.type = PType::Data;
  p.prio = kPrioHigh;
  p.mode = Mode::Geo;
  p.wide = true;
  p.hopsLeft = 12;
  p.src = 0xA1B2C3D4;
  p.dst = 0x01020304;
  p.msgId = 0xDEADBEEF;
  p.prevHop = 0x1234;
  p.nextHop = 0;
  p.hasFwdPos = true;
  p.fwdPos = {46.5, -114.25, true};
  p.hasDstPos = true;
  p.dstPos = {46.6, -114.1, true};
  p.dstAge = ageEncode(300);
  p.hasSrcPos = true;
  p.srcPos = {46.4, -114.3, true};
  p.srcAge = 0;
  p.payload = {'h', 'e', 'l', 'l', 'o'};
  return p;
}

static void testPacket() {
  std::printf("packet\n");
  Packet p = fullPacket();
  uint8_t buf[kMaxFrame];
  size_t n = encode(p, buf, sizeof buf);
  CHECK(n == 19 + 6 + 7 + 7 + 5 + 4);
  CHECK(n == encodedSize(p));
  Packet q;
  CHECK(decode(buf, n, q));
  CHECK(q.type == PType::Data && q.prio == kPrioHigh && q.mode == Mode::Geo && q.wide);
  CHECK(q.hopsLeft == 12 && q.src == p.src && q.dst == p.dst && q.msgId == p.msgId);
  CHECK(q.prevHop == 0x1234 && q.payload == p.payload);
  CHECK(std::fabs(q.fwdPos.lat - 46.5) < 2e-5 && std::fabs(q.dstPos.lon + 114.1) < 3e-5);
  CHECK(q.dstAge == p.dstAge && q.srcAge == 0);

  // mutable fields may change hop by hop without breaking the MIC
  buf[2] = static_cast<uint8_t>((3 << 4) | 1);   // hops
  buf[15] = 0x99;                                // prev_hop
  buf[19] ^= 0x55;                               // fwd_pos
  CHECK(decode(buf, n, q));
  CHECK(q.hopsLeft == 3);
  // immutable fields may not
  buf[n - 5] ^= 0x01;                            // payload byte
  CHECK(!decode(buf, n, q));

  // spray copies live in the hops low nibble
  Packet s;
  s.mode = Mode::Spray;
  s.copiesLog2 = 3;
  n = encode(s, buf, sizeof buf);
  CHECK(decode(buf, n, q) && q.copiesLog2 == 3 && q.mode == Mode::Spray);

  // too big for a frame
  Packet big;
  big.payload.assign(200, 0);
  CHECK(encode(big, buf, sizeof buf) == 0);
  CHECK(!decode(buf, 10, q));
}

static void fillKey(uint8_t* k, uint8_t seed) {
  for (int i = 0; i < 32; ++i) k[i] = static_cast<uint8_t>(seed + 7 * i);
}

static void testAead() {
  std::printf("aead + key agreement\n");
  // RFC 8439 §2.8.2: Monocypher's IETF mode is plain ChaCha20-Poly1305 (libsodium-compatible)
  {
    const char* text =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";
    size_t n = std::strlen(text);
    uint8_t key[32], nonce[12] = {0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
    uint8_t ad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(0x80 + i);
    std::vector<uint8_t> ct(n);
    uint8_t mac[16];
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    crypto_aead_write(&ctx, ct.data(), mac, ad, sizeof ad, reinterpret_cast<const uint8_t*>(text), n);
    const uint8_t ct0[8] = {0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb};
    const uint8_t tag[16] = {0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
                             0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91};
    CHECK(std::memcmp(ct.data(), ct0, 8) == 0);
    CHECK(std::memcmp(mac, tag, 16) == 0);
  }

  uint8_t key[32];
  fillKey(key, 3);
  Packet p = fullPacket();
  std::vector<uint8_t> plain = p.payload;
  CHECK(aeadSeal(p, key));
  CHECK(p.encrypted && p.payload != plain && !aeadSeal(p, key));
  uint8_t buf[kMaxFrame];
  size_t n = encode(p, buf, sizeof buf);
  CHECK(n == 19 + 6 + 7 + 7 + 5 + 16 && n == encodedSize(p));
  CHECK(buf[1] & 0x02);                          // encrypted flag is bit 1
  Packet q;
  CHECK(decode(buf, n, q) && q.encrypted && q.payload == p.payload);   // relays parse it
  std::vector<uint8_t> out;
  CHECK(aeadOpen(q, key, out) && out == plain);

  // mutable fields change hop by hop; the tag still verifies
  uint8_t m[kMaxFrame];
  std::memcpy(m, buf, n);
  m[2] = static_cast<uint8_t>((2 << 4) | 0);     // hops_left, wide bit
  m[15] = 0x77;                                  // prev_hop
  m[17] = 0x42;                                  // next_hop
  m[19] ^= 0x5A;                                 // fwd_pos
  m[25] ^= 0x33;                                 // dst_pos
  m[31] = 3;                                     // dst age
  CHECK(decode(m, n, q) && aeadOpen(q, key, out) && out == plain);
  // a custodian re-encodes it as UNICAST without fwd/dst position: still fine
  q.mode = Mode::Unicast;
  q.copiesLog2 = 2;
  q.hasFwdPos = false;
  q.hasDstPos = false;
  size_t n2 = encode(q, m, sizeof m);
  Packet r;
  CHECK(n2 == n - 13 && decode(m, n2, r) && aeadOpen(r, key, out) && out == plain);

  // tampering with anything end-to-end is rejected
  auto tampered = [&](size_t off, uint8_t mask) {
    uint8_t t[kMaxFrame];
    std::memcpy(t, buf, n);
    t[off] ^= mask;
    Packet x;
    std::vector<uint8_t> o;
    return decode(t, n, x) && !aeadOpen(x, key, o) && o.empty();
  };
  CHECK(tampered(n - 17, 0x01));                 // ciphertext
  CHECK(tampered(n - 1, 0x80));                  // tag
  CHECK(tampered(0, 0x01));                      // type
  CHECK(tampered(1, 0x40));                      // priority
  CHECK(tampered(3, 0x01));                      // src (also the nonce)
  CHECK(tampered(7, 0x01));                      // dst
  CHECK(tampered(11, 0x01));                     // msg_id (also the nonce)
  CHECK(tampered(32, 0x01));                     // src_pos
  CHECK(tampered(38, 0x01));                     // src age
  uint8_t other[32];
  fillKey(other, 4);
  CHECK(decode(buf, n, q) && !aeadOpen(q, other, out));
  // flipping the encrypted flag off makes it a (bad-MIC) plaintext frame
  uint8_t t[kMaxFrame];
  std::memcpy(t, buf, n);
  t[1] &= ~0x02;
  CHECK(!decode(t, n, q));

  // does not fit once the 16-byte tag is added
  Packet big = fullPacket();
  big.payload.assign(kMaxFrame - (19 + 6 + 7 + 7 + 4), 0);
  CHECK(encodedSize(big) == kMaxFrame && !aeadSeal(big, key) && !big.encrypted);

  // X25519 DM key: both ends derive the same key, a third party does not
  uint8_t sa[32], sb[32], sc[32], pa[32], pb[32], pc[32], ka[32], kb[32], kc[32];
  fillKey(sa, 11);
  fillKey(sb, 99);
  fillKey(sc, 50);
  x25519Public(sa, pa);
  x25519Public(sb, pb);
  x25519Public(sc, pc);
  CHECK(dmKey(sa, pb, ka) && dmKey(sb, pa, kb) && std::memcmp(ka, kb, 32) == 0);
  CHECK(dmKey(sc, pb, kc) && std::memcmp(ka, kc, 32) != 0);
  uint8_t zero[32] = {0};
  CHECK(!dmKey(sa, zero, kc));                   // low-order point
}

static void testPosAge() {
  std::printf("position + age\n");
  double worst = 0;
  for (double lat = -89; lat <= 89; lat += 7.3)
    for (double lon = -179; lon <= 179; lon += 11.1) {
      uint8_t b[6];
      Position p{lat, lon, true};
      packPos(p, b);
      worst = std::max(worst, distanceM(p, unpackPos(b)));
    }
  CHECK(worst < 3.0);
  CHECK(ageDecode(ageEncode(0)) == 0);
  CHECK(ageDecode(ageEncode(59 * 60)) == 59 * 60);
  CHECK(ageDecode(ageEncode(5 * 3600)) == 5 * 3600);
  CHECK(ageDecode(ageEncode(3 * 86400)) == 3 * 86400);
  CHECK(ageDecode(ageEncode(61 * 3600)) == 3 * 86400);
  CHECK(ageEncode(1000u * 86400u) == 254);
  CHECK(ageDecode(0xFF) == UINT32_MAX);
  uint32_t prev = 0;
  bool mono = true;
  for (uint32_t s = 0; s < 200u * 86400u; s += 977) {
    uint32_t d = ageDecode(ageEncode(s));
    if (d < prev) mono = false;
    prev = d;
  }
  CHECK(mono);
}

static void testUtil() {
  std::printf("airtime + geo + nmea\n");
  Phy phy;
  CHECK(airtimeMs(phy, 85) == 526);   // matches sim/ghostsim.py airtime(85)
  CHECK(airtimeMs(phy, 64) == 423);
  Position a{46.0, -114.0, true}, b{46.0 + 0.0089932, -114.0, true};
  CHECK(std::fabs(distanceM(a, b) - 1000) < 1.0);
  Position g;
  CHECK(parseNmea("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n", g));
  CHECK(std::fabs(g.lat - 48.1173) < 1e-4 && std::fabs(g.lon - 11.516667) < 1e-4);
  CHECK(parseNmea("$GPRMC,123519,A,4807.038,S,01131.000,W,022.4,084.4,230394,003.1,W*65", g));
  CHECK(g.lat < 0 && g.lon < 0);
  CHECK(!parseNmea("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48", g));
  CHECK(!parseNmea("$GPGGA,123519,,,,,0,00,,,M,,M,,*66", g));
}

// --------------------------------------------------- multi-node simulation --

struct Medium;

struct TestRadio : RadioPort {
  Medium* m;
  int idx;
  bool channelBusy() override { return false; }
  bool transmit(const uint8_t* buf, size_t len) override;
};

struct TestApp : AppPort {
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> got;
  std::vector<uint32_t> delivered;
  std::vector<SosInfo> sos;
  std::vector<std::pair<uint32_t, uint16_t>> sosAcks;
  std::vector<std::pair<uint32_t, std::string>> idents;   // first-seen peers
  std::vector<std::pair<uint32_t, std::array<uint8_t, 32>>> keyChanges;
  void onMessage(uint32_t src, uint32_t, const std::vector<uint8_t>& p) override { got.push_back({src, p}); }
  void onDelivered(uint32_t, uint32_t mid) override { delivered.push_back(mid); }
  void onSos(const SosInfo& s) override { sos.push_back(s); }
  void onSosAck(uint32_t who, uint16_t seq) override { sosAcks.push_back({who, seq}); }
  void onPeerIdent(uint32_t id, const std::string& name, bool first) override {
    if (first) idents.push_back({id, name});
  }
  void onKeyChange(uint32_t id, const uint8_t pub[32], const std::string&) override {
    std::array<uint8_t, 32> k;
    std::memcpy(k.data(), pub, 32);
    keyChanges.push_back({id, k});
  }
};

struct SimNode {
  TestRadio radio;
  TestApp app;
  std::unique_ptr<Router> router;
  Position pos;
};

struct Medium {
  std::vector<std::unique_ptr<SimNode>> nodes;
  double rangeM = 2500;
  uint32_t identIntervalMs = 60 * 60 * 1000;
  uint64_t now = 0;
  uint32_t txCount = 0;
  std::vector<uint8_t> txTypes;
  std::vector<std::vector<uint8_t>> frames;

  SimNode& add(uint32_t id, double kmNorth, double kmEast = 0, bool responder = false) {
    auto n = std::make_unique<SimNode>();
    n->radio.m = this;
    n->radio.idx = static_cast<int>(nodes.size());
    Config c;
    c.nodeId = id;
    c.responder = responder;
    c.rngSeed = id * 7919ull + 1;
    c.identIntervalMs = identIntervalMs;
    n->router = std::make_unique<Router>(c, n->radio, n->app);
    n->pos = {45.0 + kmNorth * 0.0089932, -110.0 + kmEast * 0.0127, true};
    n->router->setPosition(n->pos, now);
    nodes.push_back(std::move(n));
    return *nodes.back();
  }
  void move(SimNode& n, double kmNorth, double kmEast = 0) {
    n.pos = {45.0 + kmNorth * 0.0089932, -110.0 + kmEast * 0.0127, true};
    n.router->setPosition(n.pos, now);
  }
  void run(uint64_t ms, uint64_t step = 10) {
    for (uint64_t end = now + ms; now < end; now += step)
      for (auto& n : nodes) n->router->tick(now);
  }
  void deliver(int from, const uint8_t* buf, size_t len) {
    ++txCount;
    txTypes.push_back(buf[0] & 0x1F);
    frames.emplace_back(buf, buf + len);
    for (auto& n : nodes) {
      if (n->radio.idx == from) continue;
      double d = distanceM(nodes[from]->pos, n->pos);
      if (d > rangeM) continue;
      float rssi = static_cast<float>(-129 + 20 * (1 - d / rangeM));
      n->router->onReceive(buf, len, rssi, now);
    }
  }
  uint32_t count(PType t) const {
    uint32_t c = 0;
    for (auto x : txTypes) c += x == static_cast<uint8_t>(t);
    return c;
  }
};

bool TestRadio::transmit(const uint8_t* buf, size_t len) {
  m->deliver(idx, buf, len);
  return true;
}

static void testLine() {
  std::printf("router: 5-node line, GEO forwarding\n");
  Medium m;
  for (int i = 0; i < 5; ++i) m.add(100 + i, 2.0 * i);
  m.run(5000);                      // initial beacons
  uint32_t before = m.count(PType::Data);
  auto& a = *m.nodes[0];
  auto& e = *m.nodes[4];
  a.router->addKnownPosition(104, e.pos, m.now);   // from the contact card
  uint32_t mid = a.router->send(104, {'h', 'i'}, m.now);
  m.run(30000);
  CHECK(e.app.got.size() == 1);
  CHECK(!e.app.got.empty() && e.app.got[0].first == 100);
  CHECK(a.app.delivered.size() == 1 && a.app.delivered[0] == mid);
  uint32_t dataTx = m.count(PType::Data) - before;
  std::printf("  data frames on air: %u (4 hops)\n", dataTx);
  CHECK(dataTx >= 4 && dataTx <= 6);
}

static void testDtnCarry() {
  std::printf("router: void -> store -> deliver when destination comes into range\n");
  Medium m;
  auto& a = m.add(200, 0);
  auto& b = m.add(201, 2);
  auto& d = m.add(202, 9);          // out of everyone's range
  m.run(5000);
  a.router->addKnownPosition(202, d.pos, m.now);
  a.router->send(202, {'s', 'o', 's'}, m.now);
  m.run(60000);
  CHECK(d.app.got.empty());
  CHECK(b.router->storeSize() == 1);  // B is the closest: it carries
  m.move(d, 3.5);                     // D hikes toward B; its next beacon triggers delivery
  m.run(120000);
  CHECK(d.app.got.size() == 1);
  CHECK(b.router->storeSize() == 0);
  CHECK(a.app.delivered.size() == 1);
}

static void testSpray() {
  std::printf("router: unknown destination -> spray -> deliver on encounter\n");
  Medium m;
  auto& a = m.add(300, 0);
  auto& b = m.add(301, 1.5);
  auto& c = m.add(302, 20);         // far away, position unknown to anyone
  m.run(5000);
  a.router->send(302, {'y', 'o'}, m.now);
  m.run(30000);
  CHECK(b.router->storeSize() == 1);  // B got half the copies
  CHECK(c.app.got.empty());
  m.move(c, 3.0);                     // C wanders near B
  m.run(120000);
  CHECK(c.app.got.size() == 1);
}

static void testDuplicateSuppression() {
  std::printf("router: dense cluster, one forward per hop\n");
  Medium m;
  m.rangeM = 3000;
  m.add(400, 0);
  for (int i = 0; i < 8; ++i) m.add(410 + i, 1.5 + 0.1 * i, -0.4 + 0.1 * i);
  m.add(499, 4.0);
  m.run(5000);
  uint32_t before = m.count(PType::Data);
  m.nodes[0]->router->addKnownPosition(499, m.nodes.back()->pos, m.now);
  m.nodes[0]->router->send(499, {'x'}, m.now);
  m.run(30000);
  CHECK(m.nodes.back()->app.got.size() == 1);
  uint32_t dataTx = m.count(PType::Data) - before;
  std::printf("  data frames on air: %u (2 hops, 8 candidates)\n", dataTx);
  CHECK(dataTx <= 3);
}

static bool onAir(const Medium& m, const std::string& s) {
  for (auto& f : m.frames)
    if (std::search(f.begin(), f.end(), s.begin(), s.end()) != f.end()) return true;
  return false;
}

static void testEncryptedRouting() {
  std::printf("router: DM and channel AEAD over a 5-node line (relays hold no keys)\n");
  Medium m;
  for (int i = 0; i < 5; ++i) m.add(500 + i, 2.0 * i);
  auto& a = *m.nodes[0];
  auto& e = *m.nodes[4];
  uint8_t sa[32], se[32], pa[32], pe[32];
  fillKey(sa, 21);
  fillKey(se, 77);
  x25519Public(sa, pa);
  x25519Public(se, pe);
  a.router->setIdentityKey(sa);
  e.router->setIdentityKey(se);
  CHECK(a.router->addPeerKey(504, pe) && e.router->addPeerKey(500, pa));
  m.run(5000);
  a.router->addKnownPosition(504, e.pos, m.now);
  std::string secret = "meet at the saddle";
  CHECK(a.router->send(504, std::vector<uint8_t>(secret.begin(), secret.end()), m.now) != 0);
  m.run(30000);
  CHECK(e.app.got.size() == 1 && std::string(e.app.got[0].second.begin(), e.app.got[0].second.end()) == secret);
  CHECK(a.app.delivered.size() == 1);           // e2e ACK still comes back
  CHECK(!onAir(m, secret));                     // never on the air in clear
  for (int i = 1; i < 4; ++i) CHECK(m.nodes[i]->app.got.empty());

  // channel PSK: nodes 1 and 3 share it, node 2 relays without it
  uint8_t ck[32];
  fillKey(ck, 5);
  m.nodes[1]->router->setChannelKey(ck);
  m.nodes[3]->router->setChannelKey(ck);
  m.nodes[1]->router->addKnownPosition(503, m.nodes[3]->pos, m.now);
  std::string chan = "camp 2 by the lake";
  m.nodes[1]->router->send(503, std::vector<uint8_t>(chan.begin(), chan.end()), m.now);
  m.run(30000);
  CHECK(m.nodes[3]->app.got.size() == 1 && !onAir(m, chan));
  CHECK(m.nodes[1]->app.delivered.size() == 1);

  // a node with the wrong channel key cannot inject: the frame is dropped
  uint8_t bad[32];
  fillKey(bad, 6);
  m.nodes[2]->router->setChannelKey(bad);
  uint32_t authBefore = m.nodes[3]->router->stats().rxAuthFail;
  m.nodes[2]->router->addKnownPosition(503, m.nodes[3]->pos, m.now);
  m.nodes[2]->router->send(503, {'x'}, m.now);
  m.run(30000);
  CHECK(m.nodes[3]->app.got.size() == 1);
  CHECK(m.nodes[3]->router->stats().rxAuthFail > authBefore);
}

static void testSosPartitionedChain() {
  std::printf("router: SOS spreads through a partitioned 4-node chain via a carrier\n");
  Medium m;
  auto& a = m.add(600, 0);                      // in trouble
  auto& b = m.add(601, 2, 0, true);             // hiker, a responder; will carry it
  auto& c = m.add(602, 9);                      // fixed node, other partition
  auto& d = m.add(603, 11);                     // fixed node, two hops from where b goes
  m.run(5000);
  CHECK(a.router->startSos("broken leg", 40, 3, m.now));
  CHECK(!a.router->startSos("again", 40, 3, m.now + 1000));   // 1 SOS per 60 s
  m.run(10000);
  uint16_t seq = 0;
  CHECK(b.router->hasSos(600, &seq) && seq == 1);
  CHECK(b.app.sos.size() == 1 && b.app.sos[0].text == "broken leg" && b.app.sos[0].battery == 40);
  CHECK(b.app.sos[0].pos.valid && distanceM(b.app.sos[0].pos, a.pos) < 5);
  CHECK(a.app.sosAcks.size() == 1 && a.app.sosAcks[0].first == 601);   // "help acknowledged"
  CHECK(!c.router->hasSos(600) && !d.router->hasSos(600));

  m.move(b, 7.5);                               // b walks into c's range (and out of a's)
  m.run(90000);
  CHECK(c.router->hasSos(600, &seq) && seq == 1);
  CHECK(d.router->hasSos(600, &seq) && seq == 1);
  CHECK(d.app.sos.size() == 1 && d.app.sos[0].text == "broken leg");

  // b leaves long enough to become a "new" neighbor again, then returns: c
  // and d already hold seq 1, and their beacon Bloom filters say so: no offers
  m.move(b, 30);
  m.run(25 * 60 * 1000);
  m.move(b, 7.5);
  uint32_t sosBefore = m.count(PType::Sos);
  m.run(3 * 60 * 1000);
  CHECK(m.count(PType::Sos) == sosBefore);

  // b goes back to a, picks up a's newest seq and carries it over: it supersedes seq 1
  m.move(b, 2);
  m.run(6 * 60 * 1000);
  uint16_t aSeq = 0;
  CHECK(b.router->hasSos(600, &aSeq) && aSeq > 1);
  m.move(b, 30);
  m.run(25 * 60 * 1000);
  m.move(b, 7.5);
  m.run(3 * 60 * 1000);
  CHECK(c.router->hasSos(600, &seq) && seq == aSeq);
  CHECK(d.router->hasSos(600, &seq) && seq == aSeq);
  CHECK(d.router->sosCount() == 1 && d.app.sos.size() == 2);

  // stored for 24 h after the last update, then dropped
  a.router->stopSos();
  m.move(b, 30);                                // out of everyone's range
  m.run(25ull * 3600 * 1000, 1000);   // coarse steps: nobody is in range
  CHECK(d.router->sosCount() == 0 && c.router->sosCount() == 0);
}

// ------------------------------------------------------------------- IDENT --

static void hexTo(const char* hex, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    unsigned v;
    std::sscanf(hex + 2 * i, "%2x", &v);
    out[i] = static_cast<uint8_t>(v);
  }
}

// An Ed25519 seed whose node ID is usable, and that ID.
static uint32_t identity(uint8_t seed[32], uint8_t start) {
  uint8_t pub[32];
  for (uint8_t s = start;; ++s) {
    fillKey(seed, s);
    ed25519Public(seed, pub);
    if (nodeIdUsable(nodeIdOf(pub))) return nodeIdOf(pub);
  }
}

static void testIdentCrypto() {
  std::printf("ident: Ed25519, SHA-512 node ID, secret-key conversion, signed body\n");
  // FIPS 180-2 SHA-512("abc"), first 16 bytes
  uint8_t h[64], want[16];
  crypto_sha512(h, reinterpret_cast<const uint8_t*>("abc"), 3);
  hexTo("ddaf35a193617abacc417349ae204131", want, 16);
  CHECK(std::memcmp(h, want, 16) == 0);

  // RFC 8032 §7.1 test 1: monocypher-ed25519 is plain Ed25519
  uint8_t seed[32], pub[32], wantPub[32], sig[64], wantSig[64], sk[64], tmp[32];
  hexTo("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
  hexTo("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", wantPub, 32);
  hexTo("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        wantSig, 64);
  ed25519Public(seed, pub);
  CHECK(std::memcmp(pub, wantPub, 32) == 0);
  std::memcpy(tmp, seed, 32);
  crypto_ed25519_key_pair(sk, pub, tmp);
  crypto_ed25519_sign(sig, sk, nullptr, 0);
  CHECK(std::memcmp(sig, wantSig, 64) == 0);
  CHECK(crypto_ed25519_check(sig, pub, nullptr, 0) == 0);

  // node ID = first 4 bytes of SHA-512(pub), little-endian
  crypto_sha512(h, pub, 32);
  CHECK(nodeIdOf(pub) == (h[0] | h[1] << 8 | h[2] << 16 | static_cast<uint32_t>(h[3]) << 24));
  CHECK(!nodeIdUsable(0) && !nodeIdUsable(kBroadcast) && !nodeIdUsable(0x12340000) && nodeIdUsable(0x12340001));

  // Ed25519 secret -> X25519 secret agrees with the public-key conversion,
  // so two Ed25519 identities derive the same DM key
  uint8_t sa[32], sb[32], pa[32], pb[32], xa[32], xb[32], xpa[32], xpb[32], ka[32], kb[32];
  identity(sa, 31);
  identity(sb, 62);
  ed25519Public(sa, pa);
  ed25519Public(sb, pb);
  ed25519SecretToX25519(sa, xa);
  ed25519SecretToX25519(sb, xb);
  x25519Public(xa, tmp);
  ed25519ToX25519(pa, xpa);
  ed25519ToX25519(pb, xpb);
  CHECK(std::memcmp(tmp, xpa, 32) == 0);
  CHECK(dmKey(xa, xpb, ka) && dmKey(xb, xpa, kb) && std::memcmp(ka, kb, 32) == 0);

  // signed body: pub || sig || name
  uint32_t ida = nodeIdOf(pa);
  std::vector<uint8_t> body;
  CHECK(identBody(sa, "KJ7RL-1", body) && body.size() == kIdentFixed + 7);
  CHECK(std::memcmp(body.data(), pa, 32) == 0);
  uint8_t got[32];
  std::string name;
  CHECK(identCheck(body, ida, got, name) && name == "KJ7RL-1" && std::memcmp(got, pa, 32) == 0);
  CHECK(!identBody(sa, "ten bytes!", body) && identBody(sa, "nine byte", body));
  CHECK(identCheck(body, ida, got, name) && name == "nine byte");
  // the whole IDENT frame fits with a full-length name
  Packet f;
  f.type = PType::Ident;
  f.mode = Mode::Unicast;
  f.payload = body;
  CHECK(encodedSize(f) == kMaxFrame);
  auto bad = [&](size_t off, uint8_t mask, uint32_t src) {
    std::vector<uint8_t> b = body;
    if (off < b.size()) b[off] ^= mask;
    std::string n;
    return !identCheck(b, src, got, n);
  };
  CHECK(bad(0, 0, nodeIdOf(pb)));               // someone else's ID
  CHECK(bad(0, 0x01, ida));                     // key (no longer hashes to ida)
  CHECK(bad(40, 0x01, ida));                    // signature
  CHECK(bad(kIdentFixed, 0x01, ida));           // name
  std::vector<uint8_t> shortBody(body.begin(), body.begin() + kIdentFixed - 1);
  CHECK(!identCheck(shortBody, ida, got, name));
}

static void testIdentExchange() {
  std::printf("ident: two nodes, no preloaded keys, exchange IDENT then a DM\n");
  Medium m;
  uint8_t sa[32], sb[32];
  uint32_t ida = identity(sa, 1), idb = identity(sb, 100);
  auto& a = m.add(ida, 0);
  auto& b = m.add(idb, 2);
  uint8_t other[32];
  fillKey(other, 200);
  CHECK(!a.router->setSigningKey(other, "x"));            // key does not own this ID
  CHECK(!a.router->setSigningKey(sa, "name too long"));
  CHECK(a.router->setSigningKey(sa, "alice") && b.router->setSigningKey(sb, "bob"));
  CHECK(!a.router->hasPeerKey(idb) && !b.router->hasPeerKey(ida));
  m.run(20000);
  CHECK(a.router->hasPeerKey(idb) && b.router->hasPeerKey(ida));
  CHECK(a.app.idents.size() == 1 && a.app.idents[0] == std::make_pair(idb, std::string("bob")));
  CHECK(b.app.idents.size() == 1 && b.app.idents[0] == std::make_pair(ida, std::string("alice")));
  uint32_t identFrames = m.count(PType::Ident);
  std::printf("  IDENT frames on air: %u\n", identFrames);
  CHECK(identFrames >= 2 && identFrames <= 6);

  // no channel key anywhere: the DM can only be sealed with the learned key
  a.router->addKnownPosition(idb, b.pos, m.now);
  std::string secret = "water at the spring";
  CHECK(a.router->send(idb, std::vector<uint8_t>(secret.begin(), secret.end()), m.now) != 0);
  m.run(20000);
  CHECK(b.app.got.size() == 1 && std::string(b.app.got[0].second.begin(), b.app.got[0].second.end()) == secret);
  CHECK(!onAir(m, secret));
  CHECK(a.app.delivered.size() == 1);

  // low rate afterwards: one broadcast each per identIntervalMs (1 h + <=10 % jitter)
  identFrames = m.count(PType::Ident);
  m.run(2ull * 3600 * 1000, 1000);
  uint32_t later = m.count(PType::Ident) - identFrames;
  CHECK(later >= 2 && later <= 4);
}

static void testIdentRequest() {
  std::printf("ident: periodic off, keys learned by request from new neighbors\n");
  Medium m;
  m.identIntervalMs = 0;
  uint8_t s[3][32];
  uint32_t id[3];
  const char* names[3] = {"a", "b", "c"};
  for (int i = 0; i < 3; ++i) {
    id[i] = identity(s[i], static_cast<uint8_t>(10 + 40 * i));
    m.add(id[i], 0.5 * i);
    CHECK(m.nodes[i]->router->setSigningKey(s[i], names[i]));
  }
  m.run(20000);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (i != j) CHECK(m.nodes[i]->router->hasPeerKey(id[j]));
  uint32_t n = m.count(PType::Ident);
  std::printf("  IDENT frames on air: %u (3 nodes)\n", n);
  CHECK(n <= 6);
  // nothing more once everyone knows everyone
  m.run(30 * 60 * 1000, 100);
  CHECK(m.count(PType::Ident) == n);
  // an explicit request still gets an answer
  CHECK(m.nodes[0]->router->requestIdent(id[1], m.now));
  m.run(20000);
  CHECK(m.count(PType::Ident) == n + 2);
}

static void inject(SimNode& to, uint32_t src, uint32_t dst, const std::vector<uint8_t>& body, uint64_t now) {
  Packet p;
  p.type = PType::Ident;
  p.mode = Mode::Unicast;
  p.hopsLeft = 0;
  p.src = src;
  p.dst = dst;
  p.msgId = 0x5150 ^ src;
  p.prevHop = shortId(src);
  p.payload = body;
  uint8_t buf[kMaxFrame];
  size_t n = encode(p, buf, sizeof buf);
  CHECK(n > 0);
  to.router->onReceive(buf, n, -100, now);
}

static void testIdentForgery() {
  std::printf("ident: forged signature or mismatched ID rejected, key change warned\n");
  Medium m;
  uint8_t sr[32], sv[32], sx[32];
  uint32_t idr = identity(sr, 3), idv = identity(sv, 70), idx = identity(sx, 140);
  auto& r = m.add(idr, 0);
  CHECK(r.router->setSigningKey(sr, "r"));
  std::vector<uint8_t> victim, attacker;
  CHECK(identBody(sv, "victim", victim) && identBody(sx, "victim", attacker));

  // attacker signs its own key but claims the victim's ID
  inject(r, idv, kBroadcast, attacker, m.now);
  // victim's genuine body with the name changed: signature no longer matches
  std::vector<uint8_t> renamed = victim;
  renamed.back() ^= 0x20;
  inject(r, idv, kBroadcast, renamed, m.now);
  // victim's genuine body sent under the attacker's ID
  inject(r, idx, kBroadcast, victim, m.now);
  // random signature
  std::vector<uint8_t> junk = victim;
  for (size_t i = kKeySize; i < kIdentFixed; ++i) junk[i] = static_cast<uint8_t>(i * 37);
  inject(r, idv, kBroadcast, junk, m.now);
  CHECK(r.router->stats().identBad == 4 && r.router->stats().identRx == 0);
  CHECK(!r.router->hasPeerKey(idv) && !r.router->hasPeerKey(idx) && r.app.idents.empty());
  m.run(5000);
  CHECK(m.count(PType::Ident) <= 1);            // no replies to forgeries (only r's own announce)

  // the genuine one is accepted (TOFU)
  inject(r, idv, kBroadcast, victim, m.now);
  CHECK(r.router->hasPeerKey(idv) && r.app.idents.size() == 1 && r.app.idents[0].second == "victim");

  // key change: r pinned a wrong key for a real node v; v's IDENT warns and
  // does not replace it, so v's DMs fail until the host accepts the new key
  Medium m2;
  auto& r2 = m2.add(idr, 0);
  auto& v2 = m2.add(idv, 2);
  CHECK(r2.router->setSigningKey(sr, "r") && v2.router->setSigningKey(sv, "victim"));
  uint8_t xx[32], px[32];
  ed25519Public(sx, px);
  ed25519ToX25519(px, xx);
  CHECK(r2.router->addPeerKey(idv, xx));
  m2.run(20000);
  CHECK(!r2.app.keyChanges.empty() && r2.app.keyChanges[0].first == idv);
  CHECK(r2.router->stats().keyChanges >= 1 && r2.app.idents.empty());
  CHECK(v2.router->hasPeerKey(idr));
  v2.router->addKnownPosition(idr, r2.pos, m2.now);
  v2.router->send(idr, {'h', 'i'}, m2.now);
  m2.run(20000);
  CHECK(r2.app.got.empty() && r2.router->stats().rxAuthFail >= 1);
  uint8_t accept[32];
  ed25519ToX25519(r2.app.keyChanges[0].second.data(), accept);
  CHECK(r2.router->addPeerKey(idv, accept));
  v2.router->send(idr, {'h', 'i'}, m2.now);
  m2.run(20000);
  CHECK(r2.app.got.size() == 1);
}

static void testIdentRekey() {
  std::printf("ident: identity change re-derives held DM keys and stops stale IDENT\n");
  Medium m;
  uint8_t sa[32], sb[32], tmp[32], pa[32], pb[32], xa[32], xb[32];
  uint32_t ida = identity(sa, 5), idb = identity(sb, 90);
  auto& a = m.add(ida, 0);
  auto& b = m.add(idb, 2);
  ed25519Public(sa, pa);
  ed25519Public(sb, pb);
  ed25519ToX25519(pa, xa);
  ed25519ToX25519(pb, xb);
  // b pins a's key under a throwaway X25519 identity, then switches to its
  // Ed25519 one: the held key must follow
  fillKey(tmp, 77);
  b.router->setIdentityKey(tmp);
  CHECK(!b.router->requestIdent(ida, m.now));   // X25519-only: cannot send IDENT
  CHECK(b.router->addPeerKey(ida, xa));
  CHECK(b.router->setSigningKey(sb, "b"));
  CHECK(a.router->setSigningKey(sa, "a") && a.router->addPeerKey(idb, xb));
  a.router->addKnownPosition(idb, b.pos, m.now);
  b.router->addKnownPosition(ida, a.pos, m.now);
  CHECK(b.router->send(ida, {'o', 'k'}, m.now) != 0);
  m.run(20000);
  CHECK(a.app.got.size() == 1 && a.router->stats().rxAuthFail == 0);
  // dropping to an X25519-only identity stops IDENT for the old key
  b.router->setIdentityKey(tmp);
  CHECK(!b.router->requestIdent(ida, m.now));
  uint32_t before = b.router->stats().identTx;
  m.run(3ull * 3600 * 1000, 1000);
  CHECK(b.router->stats().identTx == before);
}

// MCU ports cap the duplicate-suppression and neighbor tables (bounded heap).
static void testTableCaps() {
  std::printf("router: seen / neighbor table caps\n");
  struct NullRadio : RadioPort {
    bool channelBusy() override { return false; }
    bool transmit(const uint8_t*, size_t) override { return true; }
  } radio;
  struct NullApp : AppPort {
    size_t n = 0;
    void onMessage(uint32_t, uint32_t, const std::vector<uint8_t>&) override { ++n; }
  } app;
  Config cfg;
  cfg.nodeId = 0x77;
  cfg.seenCapacity = 16;
  Router r(cfg, radio, app);
  uint8_t buf[kMaxFrame];
  uint64_t now = 1000;
  for (uint32_t i = 0; i < 100; ++i) {
    Packet p;
    p.src = 0x1000 + i;
    p.dst = 0x77;
    p.msgId = i + 1;
    p.payload = {'m'};
    size_t n = encode(p, buf, sizeof buf);
    r.onReceive(buf, n, -90, now);
    now += 50;
    r.tick(now);
  }
  CHECK(app.n == 100);
  std::printf("  seen entries after 100 messages: %zu (cap 16)\n", r.seenSize());
  CHECK(r.seenSize() <= 16);

  for (uint32_t i = 0; i < 600; ++i) {
    Packet b;
    b.type = PType::Beacon;
    b.mode = Mode::ScopedFlood;
    b.hopsLeft = 0;
    b.src = 0x20000 + i;
    b.hasFwdPos = true;
    b.fwdPos = {46.5 + i * 1e-4, -114.0, true};
    size_t n = encode(b, buf, sizeof buf);
    r.onReceive(buf, n, -90, now);
    now += 10;
  }
  std::printf("  neighbor table after 600 beaconing nodes: %zu (cap 512)\n", r.neighborTableSize());
  CHECK(r.neighborTableSize() == 512);
}

// ---------------------------------------------------------- virtual radio --

static Position kmNorth(double km) { return {45.0 + km * 0.0089932, -110.0, true}; }

static void testVirtualChannel() {
  std::printf("virtual radio: path loss, range, capture, busy\n");
  ChannelModel m;
  CHECK(std::fabs(pathLossDb(m, 1) - 31.7) < 1e-9);
  CHECK(std::fabs(pathLossDb(m, 1000) - (31.7 + 105)) < 1e-9);   // 35 dB per decade
  CHECK(pathLossDb(m, 0.1) == pathLossDb(m, 1));

  Rng r(42);
  double sum = 0, sq = 0;
  const int n = 20000;
  for (int i = 0; i < n; ++i) {
    double g = gaussian(r);
    sum += g;
    sq += g * g;
  }
  CHECK(std::fabs(sum / n) < 0.05 && std::fabs(sq / n - 1) < 0.05);

  Phy phy;
  ChannelModel flat = m;
  flat.fadeSigmaDb = 0;
  const uint8_t f[20] = {1, 2, 3};
  const uint32_t at = airtimeMs(phy, sizeof f);
  uint8_t out[kMaxFrame];
  float rssi = 0;
  {   // 1.5 km: 24 + 2 - 31.7 - 35*log10(1500) = -116.9 dBm, decoded after its airtime
    VirtualChannel c(phy, flat, 1);
    float p = c.onAir(f, sizeof f, kmNorth(0), 24, kmNorth(1.5), 1000);
    CHECK(std::fabs(p - (26 - pathLossDb(m, distanceM(kmNorth(0), kmNorth(1.5))))) < 0.01);
    CHECK(c.busy(1000) && c.busy(1000 + at - 1) && !c.busy(1000 + at));
    CHECK(c.receive(out, sizeof out, rssi, 1000 + at - 1) == 0);
    CHECK(c.receive(out, sizeof out, rssi, 1000 + at) == sizeof f);
    CHECK(out[2] == 3 && rssi == p && c.stats().decoded == 1);
    CHECK(c.receive(out, sizeof out, rssi, 1000 + at) == 0);
  }
  {   // 7 km: below the interference floor, ignored entirely
    VirtualChannel c(phy, flat, 1);
    CHECK(c.onAir(f, sizeof f, kmNorth(0), 24, kmNorth(7), 0) < flat.sensitivityDbm);
    CHECK(!c.busy(1) && c.receive(out, sizeof out, rssi, 10000) == 0);
    CHECK(c.stats().outOfRange == 1);
  }
  {   // 4 km: -134 dBm, interference only; not busy, not decoded
    VirtualChannel c(phy, flat, 1);
    float p = c.onAir(f, sizeof f, kmNorth(0), 24, kmNorth(4), 0);
    CHECK(p < flat.sensitivityDbm && p > flat.interfFloorDbm);
    CHECK(!c.busy(1) && c.receive(out, sizeof out, rssi, 10000) == 0 && c.stats().weak == 1);
  }
  {   // no position on either side: dropped
    VirtualChannel c(phy, flat, 1);
    c.onAir(f, sizeof f, Position{}, 24, kmNorth(1), 0);
    c.onAir(f, sizeof f, kmNorth(0), 24, Position{}, 0);
    CHECK(c.stats().noPos == 2 && c.receive(out, sizeof out, rssi, 10000) == 0);
  }
  {   // overlap within 6 dB: both lost; 0.5 km vs 2 km (> 20 dB apart): the strong one captures
    VirtualChannel c(phy, flat, 1);
    c.onAir(f, sizeof f, kmNorth(1.0), 24, kmNorth(0), 0);
    c.onAir(f, sizeof f, kmNorth(1.1), 24, kmNorth(0), at / 2);
    CHECK(c.receive(out, sizeof out, rssi, 3 * at) == 0 && c.stats().collided == 2);
    VirtualChannel d(phy, flat, 1);
    const uint8_t g[20] = {9};
    d.onAir(f, sizeof f, kmNorth(2.0), 24, kmNorth(0), 0);
    d.onAir(g, sizeof g, kmNorth(0.5), 24, kmNorth(0), at / 2);
    CHECK(d.receive(out, sizeof out, rssi, 3 * at) == sizeof g && out[0] == 9);
    CHECK(d.receive(out, sizeof out, rssi, 3 * at) == 0 && d.stats().collided == 1);
    // back to back, no overlap: both decoded
    VirtualChannel e(phy, flat, 1);
    e.onAir(f, sizeof f, kmNorth(1.0), 24, kmNorth(0), 0);
    e.onAir(g, sizeof g, kmNorth(1.1), 24, kmNorth(0), at);
    CHECK(e.receive(out, sizeof out, rssi, 3 * at) == sizeof f && out[0] == 1);
    CHECK(e.receive(out, sizeof out, rssi, 3 * at) == sizeof g && out[0] == 9);
  }
  {   // half duplex: our transmission kills a frame in progress and one that starts during it
    VirtualChannel c(phy, flat, 1);
    c.onAir(f, sizeof f, kmNorth(1), 24, kmNorth(0), 0);
    c.onTransmit(sizeof f, at / 2);
    CHECK(c.busy(at / 2 + at - 1) && !c.busy(at / 2 + at));
    c.onAir(f, sizeof f, kmNorth(1), 24, kmNorth(0), at);
    CHECK(c.receive(out, sizeof out, rssi, 5 * at) == 0 && c.stats().collided == 2);
  }
  {   // fading: about half the frames at the median range get through, more nearer
    auto rate = [&](double km) {
      VirtualChannel c(phy, m, 7);
      int ok = 0;
      for (int i = 0; i < 2000; ++i) {
        uint64_t t = uint64_t(i) * 2 * at;
        c.onAir(f, sizeof f, kmNorth(0), 24, kmNorth(km), t);
        if (c.receive(out, sizeof out, rssi, t + at)) ++ok;
      }
      return ok / 2000.0;
    };
    double median = std::pow(10, (26 - 31.7 + 129) / 35) / 1000;   // ~3.3 km
    CHECK(std::fabs(rate(median) - 0.5) < 0.05);
    CHECK(rate(1.5) > 0.98 && rate(8) < 0.02);
  }
  {   // bounded: more than 16 frames at once are counted, not stored
    VirtualChannel c(phy, flat, 1);
    for (int i = 0; i < 20; ++i) c.onAir(f, sizeof f, kmNorth(1), 24, kmNorth(0), 0);
    CHECK(c.stats().overflow == 4);
  }
}

int main() {
  testPacket();
  testAead();
  testPosAge();
  testUtil();
  testLine();
  testDtnCarry();
  testSpray();
  testDuplicateSuppression();
  testEncryptedRouting();
  testSosPartitionedChain();
  testIdentCrypto();
  testIdentExchange();
  testIdentRequest();
  testIdentForgery();
  testIdentRekey();
  testTableCaps();
  testVirtualChannel();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
