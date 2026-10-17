// Host tests for ghostwaved's virtual radio (pi/vchannel.*). A separate binary
// so the core tests in test_main.cpp do not depend on daemon code.
#include <cmath>
#include <cstdio>

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

static void testRepeatFilter() {
  std::printf("virtual radio: a datagram looped back twice is dropped once\n");
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
  testVirtualChannel();
  testRepeatFilter();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
