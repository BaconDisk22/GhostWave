// Host tests for the virtual radio's channel model (pi/vchannel.*). Separate
// from test_main.cpp so the core tests stay free of ghostwaved code.
#include <cmath>
#include <cstdio>

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

static const Position kHere{46.5, -114.0, true};

// `m` metres due north of kHere.
static Position north(double m) { return {kHere.lat + m / 111195.0, kHere.lon, true}; }

static ChannelModel noFade() {
  ChannelModel m;
  m.fadeSigmaDb = 0;
  return m;
}

static void testPathLoss() {
  std::printf("vchannel: path loss and range match ghostsim\n");
  ChannelModel m;
  CHECK(std::fabs(pathLossDb(m, 1) - 31.7) < 1e-9);
  CHECK(std::fabs(pathLossDb(m, 0.2) - 31.7) < 1e-9);
  CHECK(std::fabs(pathLossDb(m, 1000) - 136.7) < 1e-9);

  // 24 dBm EIRP + 2 dBi: decodable to ~3.3 km, interference to ~4.7 km
  Phy phy;
  VirtualChannel ch(phy, noFade(), 1);
  uint8_t f[20] = {1, 2, 3};
  float p = ch.onAir(f, sizeof f, north(1000), 24, kHere, 0);
  CHECK(std::fabs(p - (26 - 136.7f)) < 0.05f);
  ch.onAir(f, sizeof f, north(3000), 24, kHere, 10000);
  ch.onAir(f, sizeof f, north(4000), 24, kHere, 20000);
  ch.onAir(f, sizeof f, north(6000), 24, kHere, 30000);
  CHECK(ch.stats().weak == 1 && ch.stats().outOfRange == 1);
  uint8_t out[kMaxFrame];
  float rssi = 0;
  int got = 0;
  while (ch.receive(out, sizeof out, rssi, 40000)) ++got;
  CHECK(got == 2 && ch.stats().decoded == 2);
}

static void testAirtimeAndBusy() {
  std::printf("vchannel: busy for the frame's airtime, delivered after it\n");
  Phy phy;
  VirtualChannel ch(phy, noFade(), 1);
  uint8_t f[40];
  for (size_t i = 0; i < sizeof f; ++i) f[i] = static_cast<uint8_t>(i);
  uint32_t air = airtimeMs(phy, sizeof f);
  CHECK(!ch.busy(1000));
  ch.onAir(f, sizeof f, north(500), 24, kHere, 1000);
  CHECK(ch.busy(1000) && ch.busy(1000 + air - 1));
  uint8_t out[kMaxFrame];
  float rssi = 0;
  CHECK(ch.receive(out, sizeof out, rssi, 1000 + air - 1) == 0);
  CHECK(!ch.busy(1000 + air));
  CHECK(ch.receive(out, sizeof out, rssi, 1000 + air) == sizeof f);
  CHECK(out[39] == 39 && rssi > -110 && rssi < -80);
  CHECK(ch.receive(out, sizeof out, rssi, 1000 + air) == 0);

  // too weak to decode: not busy either (CAD would not see it)
  ch.onAir(f, sizeof f, north(4000), 24, kHere, 5000);
  CHECK(!ch.busy(5001));
  // our own transmission
  ch.onTransmit(10, 9000);
  CHECK(ch.busy(9000 + airtimeMs(phy, 10) - 1) && !ch.busy(9000 + airtimeMs(phy, 10)));
}

static void testCollisions() {
  std::printf("vchannel: capture, collisions, half duplex\n");
  Phy phy;
  uint8_t a[20] = {0xA}, b[20] = {0xB}, out[kMaxFrame];
  float rssi = 0;
  {  // equal power, overlapping: both lost
    VirtualChannel ch(phy, noFade(), 1);
    ch.onAir(a, sizeof a, north(1000), 24, kHere, 0);
    ch.onAir(b, sizeof b, north(1000), 24, kHere, 50);
    CHECK(ch.receive(out, sizeof out, rssi, 10000) == 0 && ch.stats().collided == 2);
  }
  {  // 10+ dB stronger: captured
    VirtualChannel ch(phy, noFade(), 1);
    ch.onAir(a, sizeof a, north(2000), 24, kHere, 0);
    ch.onAir(b, sizeof b, north(500), 24, kHere, 50);
    CHECK(ch.receive(out, sizeof out, rssi, 10000) == sizeof b && out[0] == 0xB);
    CHECK(ch.receive(out, sizeof out, rssi, 10000) == 0 && ch.stats().collided == 1);
  }
  {  // a weak-but-audible frame still interferes
    VirtualChannel ch(phy, noFade(), 1);
    ch.onAir(a, sizeof a, north(3000), 24, kHere, 0);
    ch.onAir(b, sizeof b, north(3500), 24, kHere, 50);
    CHECK(ch.receive(out, sizeof out, rssi, 10000) == 0);
  }
  {  // back to back: no overlap, both received
    VirtualChannel ch(phy, noFade(), 1);
    ch.onAir(a, sizeof a, north(1000), 24, kHere, 0);
    ch.onAir(b, sizeof b, north(1000), 24, kHere, airtimeMs(phy, sizeof a));
    CHECK(ch.receive(out, sizeof out, rssi, 10000) == sizeof a && out[0] == 0xA);
    CHECK(ch.receive(out, sizeof out, rssi, 10000) == sizeof b && out[0] == 0xB);
  }
  {  // we transmit mid-frame, or a frame starts while we transmit: lost
    VirtualChannel ch(phy, noFade(), 1);
    ch.onAir(a, sizeof a, north(1000), 24, kHere, 0);
    ch.onTransmit(10, 20);
    ch.onAir(b, sizeof b, north(1000), 24, kHere, 30);
    CHECK(ch.receive(out, sizeof out, rssi, 10000) == 0 && ch.stats().collided == 2);
  }
  {  // no position on either side: dropped
    VirtualChannel ch(phy, noFade(), 1);
    ch.onAir(a, sizeof a, Position{}, 24, kHere, 0);
    ch.onAir(a, sizeof a, north(10), 24, Position{}, 0);
    CHECK(ch.stats().noPos == 2 && ch.receive(out, sizeof out, rssi, 10000) == 0);
  }
  {  // bounded: more overlapping frames than slots are counted, not stored
    VirtualChannel ch(phy, noFade(), 1);
    for (int i = 0; i < 20; ++i) ch.onAir(a, sizeof a, north(100 + 150 * i), 24, kHere, i);
    CHECK(ch.stats().overflow == 4);
  }
}

static void testFading() {
  std::printf("vchannel: 5 dB lognormal fading\n");
  Rng rng(7);
  double sum = 0, sq = 0;
  const int n = 20000;
  for (int i = 0; i < n; ++i) {
    double g = gaussian(rng);
    sum += g;
    sq += g * g;
  }
  double mean = sum / n, sd = std::sqrt(sq / n - mean * mean);
  CHECK(std::fabs(mean) < 0.03 && std::fabs(sd - 1) < 0.03);

  // near the edge some frames make it and some do not
  Phy phy;
  VirtualChannel ch(phy, ChannelModel{}, 3);
  uint8_t f[10] = {}, out[kMaxFrame];
  float rssi = 0;
  int got = 0;
  for (int i = 0; i < 200; ++i) {
    ch.onAir(f, sizeof f, north(3300), 24, kHere, i * 1000ull);
    got += ch.receive(out, sizeof out, rssi, i * 1000ull + 999) ? 1 : 0;
  }
  CHECK(got > 50 && got < 150);
}

static void testRepeatFilter() {
  std::printf("vchannel: a datagram looped back twice is dropped once\n");
  RepeatFilter rf;
  CHECK(!rf.repeat(0xA, 1));
  CHECK(rf.repeat(0xA, 1));
  CHECK(!rf.repeat(0xB, 1));   // same counter, other sender
  CHECK(!rf.repeat(0xA, 2));
  CHECK(!rf.repeat(0, 0));     // an all-zero datagram is not a repeat of an empty slot
  for (uint32_t i = 100; i < 200; ++i) rf.repeat(0xC, i);
  CHECK(!rf.repeat(0xA, 1));   // long forgotten
}

int main() {
  testPathLoss();
  testAirtimeAndBusy();
  testCollisions();
  testFading();
  testRepeatFilter();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
