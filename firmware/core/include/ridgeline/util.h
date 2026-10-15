// Geometry, LoRa airtime, a tiny RNG, and an NMEA parser.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ridgeline/packet.h"

namespace rl {

// Great-circle distance in metres.
double distanceM(const Position& a, const Position& b);

// Point `frac` of the way from a to b (linear in lat/lon; fine below ~50 km).
Position lerp(const Position& a, const Position& b, double frac);

struct Phy {
  int sf = 9;
  double bwHz = 125000;
  int cr = 1;           // 1 = 4/5
  int preamble = 16;
  float sensitivityDbm = -129.0f;
  double maxRangeM = 6000;   // R_est: normalizes CBF progress (DESIGN §5.2)
};

// LoRa time on air, explicit header, CRC on.
uint32_t airtimeMs(const Phy& phy, size_t bytes);

class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    return static_cast<uint32_t>(s_ >> 11);
  }
  uint32_t range(uint32_t lo, uint32_t hi) { return hi <= lo ? lo : lo + next() % (hi - lo); }

 private:
  uint64_t s_;
};

// Parses $GxGGA / $GxRMC. Returns true and fills `out` when the sentence is a
// valid fix with a correct checksum.
bool parseNmea(const std::string& line, Position& out);

}  // namespace rl
