// Receiver side of the virtual radio: decides, per frame heard on the wire,
// whether this node would have decoded it over the air. Same channel model as
// sim/ghostsim.py: log-distance path loss (PL0 31.7 dB, exponent 3.5), 5 dB
// Gaussian fading per frame, -129 dBm sensitivity, 6 dB capture, half duplex.
//
// No sockets and no clock here, so the host tests can drive it directly.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ghostwave/packet.h"
#include "ghostwave/util.h"

namespace gw {

struct ChannelModel {
  double pl0Db = 31.7;           // path loss at 1 m
  double plExp = 3.5;
  double fadeSigmaDb = 5.0;      // 0 = deterministic
  float sensitivityDbm = -129.0f;
  float captureDb = 6.0f;        // stronger frame survives a collision by this margin
  float interfFloorDbm = -135.0f;   // weaker than this: not even interference
  float rxGainDb = 2.0f;
};

// Log-distance path loss in dB; distances under 1 m count as 1 m.
double pathLossDb(const ChannelModel& m, double distM);

// Standard normal sample from `rng` (Box–Muller).
double gaussian(Rng& rng);

struct ChannelStats {
  uint32_t heard = 0;        // frames from other nodes seen on the wire
  uint32_t outOfRange = 0;   // below the interference floor
  uint32_t weak = 0;         // above the floor, below sensitivity
  uint32_t collided = 0;     // lost to capture or to our own transmission
  uint32_t noPos = 0;        // sender or receiver had no position
  uint32_t overflow = 0;     // more frames on the air than we track
  uint32_t decoded = 0;
};

class VirtualChannel {
 public:
  VirtualChannel(const Phy& phy, const ChannelModel& model, uint64_t seed);

  // A frame another node started sending at `nowMs`. `txDbm` is the sender's
  // EIRP (power plus antenna gain). Returns the received power in dBm, or a
  // value below interfFloorDbm when the frame is ignored.
  float onAir(const uint8_t* frame, size_t len, const Position& txPos, float txDbm,
              const Position& myPos, uint64_t nowMs);
  // We start sending a frame of `len` bytes: anything on the air is lost.
  void onTransmit(size_t len, uint64_t nowMs);
  // True while any frame above sensitivity, or our own transmission, is on the air.
  bool busy(uint64_t nowMs) const;
  // Pops one frame whose airtime has ended and that survived. Returns its
  // length (0 if none ready) and fills `rssiDbm`.
  size_t receive(uint8_t* out, size_t cap, float& rssiDbm, uint64_t nowMs);

  const ChannelStats& stats() const { return stats_; }

 private:
  struct Rec {
    bool used = false;
    bool lost = false;
    bool decodable = false;
    uint64_t start = 0, end = 0;
    float powerDbm = 0;
    uint8_t len = 0;
    std::array<uint8_t, kMaxFrame> buf{};
  };
  static constexpr size_t kMaxRecs = 16;

  void expire(uint64_t nowMs);

  Phy phy_;
  ChannelModel model_;
  Rng rng_;
  ChannelStats stats_;
  uint64_t txUntil_ = 0;
  std::array<Rec, kMaxRecs> recs_{};
};

// Drops a datagram seen twice. macOS loops multicast back on more than one
// path, so one transmission can arrive twice; the copies would then collide
// with each other in VirtualChannel. Senders number their datagrams.
class RepeatFilter {
 public:
  // True if (src, seq) was among the last kSize datagrams; records it if not.
  bool repeat(uint32_t src, uint32_t seq);

 private:
  static constexpr size_t kSize = 64;
  struct Entry {
    bool used = false;
    uint32_t src = 0, seq = 0;
  };
  std::array<Entry, kSize> recent_{};
  size_t next_ = 0;
};

}  // namespace gw
