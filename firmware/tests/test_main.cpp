// Host tests for the GhostWave core. No framework: each CHECK prints and counts.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "monocypher.h"
#include "ghostwave/crypto.h"
#include "ghostwave/packet.h"
#include "ghostwave/router.h"
#include "ghostwave/util.h"

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
  void onMessage(uint32_t src, uint32_t, const std::vector<uint8_t>& p) override { got.push_back({src, p}); }
  void onDelivered(uint32_t, uint32_t mid) override { delivered.push_back(mid); }
  void onSos(const SosInfo& s) override { sos.push_back(s); }
  void onSosAck(uint32_t who, uint16_t seq) override { sosAcks.push_back({who, seq}); }
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
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
