#include "vchannel.h"

#include <cmath>
#include <cstring>

namespace gw {

double pathLossDb(const ChannelModel& m, double distM) {
  return m.pl0Db + 10.0 * m.plExp * std::log10(distM < 1.0 ? 1.0 : distM);
}

double gaussian(Rng& rng) {
  // next() yields 21 bits; offset by one half so u1 is never 0
  double u1 = (rng.next() % (1u << 21) + 0.5) / double(1u << 21);
  double u2 = (rng.next() % (1u << 21) + 0.5) / double(1u << 21);
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
}

VirtualChannel::VirtualChannel(const Phy& phy, const ChannelModel& model, uint64_t seed)
    : phy_(phy), model_(model), rng_(seed) {}

void VirtualChannel::expire(uint64_t nowMs) {
  for (Rec& r : recs_) {
    if (!r.used || r.end > nowMs || (r.decodable && !r.lost)) continue;
    if (r.decodable) ++stats_.collided;
    r.used = false;
  }
}

float VirtualChannel::onAir(const uint8_t* frame, size_t len, const Position& txPos, float txDbm,
                            const Position& myPos, uint64_t nowMs) {
  ++stats_.heard;
  const float ignored = model_.interfFloorDbm - 1;
  if (!txPos.valid || !myPos.valid) {
    ++stats_.noPos;
    return ignored;
  }
  if (len == 0 || len > kMaxFrame) return ignored;
  double fade = model_.fadeSigmaDb > 0 ? gaussian(rng_) * model_.fadeSigmaDb : 0.0;
  float p = static_cast<float>(txDbm + model_.rxGainDb -
                               pathLossDb(model_, distanceM(txPos, myPos)) + fade);
  if (p < model_.interfFloorDbm) {
    ++stats_.outOfRange;
    return p;
  }
  expire(nowMs);
  Rec* slot = nullptr;
  for (Rec& r : recs_)
    if (!r.used) {
      slot = &r;
      break;
    }
  if (!slot) {
    ++stats_.overflow;
    return ignored;
  }
  Rec& n = *slot;
  n = Rec{};
  n.used = true;
  n.start = nowMs;
  n.end = nowMs + airtimeMs(phy_, len);
  n.powerDbm = p;
  n.decodable = p >= model_.sensitivityDbm;
  n.len = static_cast<uint8_t>(len);
  std::memcpy(n.buf.data(), frame, len);
  if (txUntil_ > nowMs) n.lost = true;   // half duplex: we are talking
  for (Rec& r : recs_) {
    if (&r == &n || !r.used || r.end <= nowMs) continue;
    if (r.powerDbm - p < model_.captureDb) r.lost = true;
    if (p - r.powerDbm < model_.captureDb) n.lost = true;
  }
  if (!n.decodable) ++stats_.weak;
  return p;
}

void VirtualChannel::onTransmit(size_t len, uint64_t nowMs) {
  for (Rec& r : recs_)
    if (r.used && r.end > nowMs) r.lost = true;
  uint64_t end = nowMs + airtimeMs(phy_, len);
  if (end > txUntil_) txUntil_ = end;
}

bool VirtualChannel::busy(uint64_t nowMs) const {
  if (txUntil_ > nowMs) return true;
  for (const Rec& r : recs_)
    if (r.used && r.decodable && r.start <= nowMs && r.end > nowMs) return true;
  return false;
}

size_t VirtualChannel::receive(uint8_t* out, size_t cap, float& rssiDbm, uint64_t nowMs) {
  expire(nowMs);   // what is left and has ended decoded cleanly
  Rec* best = nullptr;
  for (Rec& r : recs_)
    if (r.used && r.end <= nowMs && (!best || r.end < best->end)) best = &r;
  if (!best) return 0;
  best->used = false;
  if (best->len > cap) return 0;
  std::memcpy(out, best->buf.data(), best->len);
  rssiDbm = best->powerDbm;
  ++stats_.decoded;
  return best->len;
}

}  // namespace gw
