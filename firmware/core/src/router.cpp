#include "ghostwave/router.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ghostwave/crypto.h"

namespace gw {
namespace {

constexpr uint64_t kSeenLifetimeMs = 3600 * 1000;
constexpr uint64_t kNbFreshMs = 20 * 60 * 1000;
constexpr uint32_t kJitterMs = 120;
constexpr uint64_t kTickMs = 30 * 1000;
constexpr size_t kLocCapacity = 512;
constexpr size_t kGossipEntries = 3;
constexpr size_t kGossipSize = 11;
constexpr size_t kBeaconFixed = 13;   // flags, velocity, util, store count, bloom
constexpr size_t kBloomOff = 5;
constexpr uint8_t kBeaconHasGps = 0x01;
constexpr uint8_t kBeaconSos = 0x10;  // sender holds at least one SOS
constexpr uint64_t kIdentAskGapMs = 10 * 60 * 1000;   // ask one neighbor at most this often
constexpr uint64_t kIdentReplyGapMs = 15 * 1000;      // our IDENTs are at least this far apart
constexpr size_t kIdentWantCapacity = 8;
constexpr size_t kIdentAskedCapacity = 32;
constexpr size_t kSosHeader = 4;      // seq (2), accuracy, battery
constexpr size_t kOfferedCapacity = 32;
constexpr uint64_t kSosReofferMs = 15 * 60 * 1000;   // to a neighbor met again
constexpr uint64_t kSosRetryMs = 60 * 1000;   // first re-offer to a known neighbor still lacking it
constexpr uint8_t kSosRetryMax = 4;           // probes after an offer: 60, 120, 240, 480 s
constexpr size_t kSosProbeCapacity = 8;
constexpr uint64_t kSosRelayGapMs = 15 * 60 * 1000;   // re-flood a superseding seq at most this often
constexpr uint64_t kBeaconReplyMinMs = 10 * 1000;
constexpr uint8_t kSosDupCancel = 2;  // counter-based flood suppression

double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

// Re-offer gap to a known neighbor after `tries` re-offers: doubles, capped.
uint64_t sosRetryGap(uint8_t tries) {
  return std::min<uint64_t>(kSosRetryMs << std::min<uint8_t>(tries, 8), kSosReofferMs);
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
uint32_t get32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Two bit positions (k = 2) in the 64-bit beacon Bloom filter for (src, msg_id).
uint32_t bloomHash(uint32_t src, uint32_t mid) {
  uint8_t b[4];
  for (int i = 0; i < 4; ++i) b[i] = (mid >> (8 * i)) & 0xFF;
  return crc32c(b, 4, src);
}
void bloomAdd(uint8_t* bloom, uint32_t src, uint32_t mid) {
  uint32_t h = bloomHash(src, mid);
  bloom[(h & 63) / 8] |= 1 << (h & 7);
  bloom[((h >> 6) & 63) / 8] |= 1 << ((h >> 6) & 7);
}
bool bloomHas(const uint8_t* bloom, uint32_t src, uint32_t mid) {
  uint32_t h = bloomHash(src, mid);
  return (bloom[(h & 63) / 8] & (1 << (h & 7))) && (bloom[((h >> 6) & 63) / 8] & (1 << ((h >> 6) & 7)));
}

// seq numbers wrap: a is newer than b if it is ahead by less than half the space
bool seqNewer(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) > 0; }

}  // namespace

Router::Router(const Config& cfg, RadioPort& radio, AppPort& app)
    : cfg_(cfg), radio_(radio), app_(app),
      rng_(cfg.rngSeed ? cfg.rngSeed : (0xA5A5ull << 32) ^ cfg.nodeId),
      tokensMs_(cfg.budgetDepthMs) {}

Router::~Router() {
  wipe(idKey_.data(), idKey_.size());
  wipe(chanKey_.data(), chanKey_.size());
  for (auto& [id, pr] : peers_) wipe(pr.dm.data(), pr.dm.size());
}

// -------------------------------------------------------------------- keys --

// A new identity drops any IDENT we were sending (it would advertise the old
// key) and re-derives the DM keys of peers already held.
void Router::setIdentityKey(const uint8_t secret[32]) {
  std::memcpy(idKey_.data(), secret, 32);
  haveIdKey_ = true;
  haveSigKey_ = false;
  identBody_.clear();
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (dmKey(idKey_.data(), it->second.xPub.data(), it->second.dm.data())) {
      ++it;
    } else {
      wipe(it->second.dm.data(), it->second.dm.size());
      it = peers_.erase(it);
    }
  }
}

void Router::setChannelKey(const uint8_t key[32]) {
  std::memcpy(chanKey_.data(), key, 32);
  haveChanKey_ = true;
}

bool Router::setSigningKey(const uint8_t seed[32], const std::string& name) {
  uint8_t pub[32], x[32];
  std::vector<uint8_t> body;
  ed25519Public(seed, pub);
  if (nodeIdOf(pub) != cfg_.nodeId || !nodeIdUsable(cfg_.nodeId) || !identBody(seed, name, body))
    return false;
  ed25519SecretToX25519(seed, x);
  setIdentityKey(x);
  wipe(x, sizeof x);
  identBody_ = std::move(body);
  haveSigKey_ = true;
  return true;
}

bool Router::addPeerKey(uint32_t id, const uint8_t pub[32]) { return storePeer(id, pub, true, 0); }

// Pinned keys replace whatever we hold; learned ones only fill a free slot (or
// evict the oldest learned key). The caller does the TOFU comparison.
bool Router::storePeer(uint32_t id, const uint8_t pub[32], bool pinned, uint64_t now) {
  Peer pr{};
  if (!haveIdKey_ || !dmKey(idKey_.data(), pub, pr.dm.data())) return false;
  if (!peers_.count(id) && peers_.size() >= cfg_.peerCapacity) {
    auto victim = peers_.end();
    for (auto it = peers_.begin(); it != peers_.end(); ++it)
      if (!it->second.pinned && (victim == peers_.end() || it->second.learnedMs < victim->second.learnedMs))
        victim = it;
    if (victim == peers_.end()) {
      wipe(pr.dm.data(), pr.dm.size());
      return false;
    }
    wipe(victim->second.dm.data(), victim->second.dm.size());
    peers_.erase(victim);
  }
  std::memcpy(pr.xPub.data(), pub, 32);
  pr.pinned = pinned;
  pr.learnedMs = now;
  auto old = peers_.find(id);
  if (old != peers_.end()) {
    pr.name = old->second.name;
    wipe(old->second.dm.data(), old->second.dm.size());
  }
  peers_[id] = pr;
  wipe(pr.dm.data(), pr.dm.size());
  identWant_.erase(id);
  return true;
}

bool Router::openData(const Packet& p, std::vector<uint8_t>& plain) {
  auto pr = peers_.find(p.src);
  if (pr != peers_.end() && aeadOpen(p, pr->second.dm.data(), plain)) return true;
  return haveChanKey_ && aeadOpen(p, chanKey_.data(), plain);
}

// ------------------------------------------------------------------ inputs --

void Router::setPosition(const Position& p, uint64_t now) {
  (void)now;
  pos_ = p;
  havePos_ = p.valid;
}

void Router::addKnownPosition(uint32_t id, const Position& p, uint64_t obsMs) { learn(id, p, obsMs); }

uint32_t Router::send(uint32_t dst, const std::vector<uint8_t>& payload, uint64_t now) {
  Packet p;
  p.type = PType::Data;
  p.src = cfg_.nodeId;
  p.dst = dst;
  p.msgId = rng_.next() | 1;
  p.hopsLeft = cfg_.hopLimit;
  p.payload = payload;
  if (havePos_) {
    p.hasSrcPos = true;
    p.srcPos = pos_;
    p.srcAge = 0;
  }
  // seal now: every field the AEAD covers is final from here on
  auto pr = peers_.find(dst);
  if (pr != peers_.end()) {
    if (!aeadSeal(p, pr->second.dm.data())) return 0;
  } else if (haveChanKey_) {
    if (!aeadSeal(p, chanKey_.data())) return 0;
  }
  Key k = keyOf(p);
  seen_[k].t = now;
  auto it = loc_.find(dst);
  if (havePos_ && it != loc_.end()) {
    p.hasDstPos = true;
    p.dstPos = it->second.pos;
    p.dstAge = ageEncode(static_cast<uint32_t>((now - std::min(now, it->second.obsMs)) / 1000));
    geoSend(p, now);
  } else {
    storeMsg(p, true, 1 << cfg_.sprayCopiesLog2, now);
    auto se = store_.find(k);
    for (auto& [id, n] : nb_) {
      if (se == store_.end() || se->second.copies <= 1) break;
      if (now - n.heardMs > 5 * 60 * 1000) continue;
      if (id == dst) {
        unicast(p, shortId(id), 1, now);
        break;
      }
      se->second.given.insert(shortId(id));
      unicast(p, shortId(id), se->second.copies / 2, now);
    }
  }
  return p.msgId;
}

void Router::onReceive(const uint8_t* buf, size_t len, float rssi, uint64_t now) {
  heardAirMs_ += airtimeMs(cfg_.phy, len);
  Packet p;
  if (!decode(buf, len, p)) {
    ++stats_.rxBad;
    return;
  }
  ++stats_.rx;
  if (p.prevHop == me16() && p.src == cfg_.nodeId && p.type == PType::Beacon) return;
  if (p.hasSrcPos && p.srcAge != 0xFF && p.src != cfg_.nodeId) {
    uint64_t age = ageDecode(p.srcAge) * 1000ull;
    learn(p.src, p.srcPos, now > age ? now - age : 0);
  }
  switch (p.type) {
    case PType::Beacon: onBeacon(p, now); break;
    case PType::LinkAck: onLinkAck(p, now); break;
    case PType::Ack: onAck(p, now); break;
    case PType::Data: onData(p, rssi, now); break;
    case PType::Sos: onSos(p, now); break;
    case PType::SosAck: onSosAck(p, now); break;
    case PType::Ident: onIdent(p, now); break;
    default: break;
  }
}

// -------------------------------------------------------------------- tick --

void Router::tick(uint64_t now) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.fireAt > now) {
      ++it;
      continue;
    }
    Pending pd = std::move(it->second);
    Key k = it->first;
    it = pending_.erase(it);
    seen_[k].custodyUp = pd.upstream;
    fresherTarget(pd.pkt, now);
    ++stats_.forwarded;
    geoSend(pd.pkt, now);
  }

  std::vector<Key> expired;
  for (auto& [k, a] : awaiting_)
    if (a.deadline <= now) expired.push_back(k);
  for (const Key& k : expired) {
    auto it = awaiting_.find(k);
    if (it == awaiting_.end()) continue;
    if (!it->second.item->sent) {           // still queued behind CSMA/budget
      it->second.deadline = now + 500;
      continue;
    }
    Await a = std::move(it->second);
    awaiting_.erase(it);
    if (a.triesLeft > 0 || !a.wide) {
      // a failed narrow-area attempt always gets one wide retry
      geoSend(a.orig, now, std::max(1, a.triesLeft), true);
    } else if (a.orig.type == PType::Data) {
      storeMsg(a.orig, false, 1, now);
    }
  }

  std::vector<std::pair<Key, uint16_t>> uexp;
  for (auto& [k, u] : uni_)
    if (u.deadline <= now) uexp.push_back(k);
  for (auto& uk : uexp) {
    auto it = uni_.find(uk);
    UniAwait& u = it->second;
    if (!u.item->sent) {
      u.deadline = now + 500;
      continue;
    }
    if (u.triesLeft <= 0) {
      auto se = store_.find(uk.first);
      if (se != store_.end()) se->second.given.erase(uk.second);
      uni_.erase(it);
      continue;
    }
    --u.triesLeft;
    u.item = enqueue(u.item->pkt, u.item->own, now);
    u.deadline = now + 3 * airtimeMs(cfg_.phy, encodedSize(u.item->pkt)) + 1500;
  }

  if (now >= nextBeaconCheck_) {
    nextBeaconCheck_ = now + kTickMs;
    beaconTick(now);
  }
  if (beaconSoonAt_ && now >= beaconSoonAt_) {
    beaconSoonAt_ = 0;
    if (havePos_ && (!beaconed_ || now - lastBeaconT_ >= kBeaconReplyMinMs)) sendBeacon(now);
  }
  sosTick(now);
  identTick(now);

  for (auto it = store_.begin(); it != store_.end();)
    it = (now - it->second.created > cfg_.storeTtlMs) ? store_.erase(it) : std::next(it);
  for (auto it = seen_.begin(); it != seen_.end();)
    it = (now - it->second.t > kSeenLifetimeMs && !pending_.count(it->first) &&
          !awaiting_.count(it->first) && !store_.count(it->first))
             ? seen_.erase(it)
             : std::next(it);

  serviceMac(now);
}

// --------------------------------------------------------------------- MAC --

Router::TxPtr Router::enqueue(const Packet& p, bool own, uint64_t now) {
  auto item = std::make_shared<TxItem>();
  item->pkt = p;
  item->own = own;
  item->readyAt = now;
  item->seq = ++txSeq_;
  txq_.push_back(item);
  return item;
}

double Router::budgetRate(uint64_t now) const {
  return std::max(cfg_.budgetFloor, cfg_.budgetShare / (neighborCount(now) + 1.0));
}

void Router::serviceMac(uint64_t now) {
  txq_.erase(std::remove_if(txq_.begin(), txq_.end(), [](const TxPtr& t) { return t->cancelled; }),
             txq_.end());
  if (now < macBackoffUntil_) return;
  TxPtr best;
  for (auto& t : txq_) {
    if (t->readyAt > now) continue;
    if (!best || t->pkt.prio > best->pkt.prio ||
        (t->pkt.prio == best->pkt.prio && t->seq < best->seq))
      best = t;
  }
  if (!best) return;
  uint8_t buf[kMaxFrame];
  size_t len = encode(best->pkt, buf, sizeof buf);
  if (len == 0) {       // cannot fit: drop
    best->cancelled = true;
    return;
  }
  uint32_t dur = airtimeMs(cfg_.phy, len);
  if (best->own && best->pkt.prio < kPrioSos) {
    double rate = budgetRate(now);
    tokensMs_ = std::min(cfg_.budgetDepthMs, tokensMs_ + (now - tokensT_) * rate);
    tokensT_ = now;
    if (tokensMs_ < dur) {
      best->readyAt = now + static_cast<uint64_t>((dur - tokensMs_) / rate) + 1;
      return;
    }
    tokensMs_ -= dur;
  }
  if (radio_.channelBusy()) {
    macBackoffUntil_ = now + rng_.range(50, 600);
    return;
  }
  txq_.erase(std::find(txq_.begin(), txq_.end(), best));
  if (radio_.transmit(buf, len)) {
    best->sent = true;
    ++stats_.tx;
    stats_.airMs += dur;
  }
  macBackoffUntil_ = now + dur;   // radio is half duplex
}

// ---------------------------------------------------------------- location --

void Router::learn(uint32_t id, const Position& p, uint64_t obsMs) {
  if (id == cfg_.nodeId || !p.valid) return;
  auto it = loc_.find(id);
  if (it != loc_.end()) {
    if (it->second.obsMs < obsMs) it->second = {p, obsMs};
    return;
  }
  if (loc_.size() >= kLocCapacity) {
    auto oldest = std::min_element(loc_.begin(), loc_.end(), [](auto& a, auto& b) {
      return a.second.obsMs < b.second.obsMs;
    });
    loc_.erase(oldest);
  }
  loc_[id] = {p, obsMs};
}

bool Router::fresherTarget(Packet& p, uint64_t now) const {
  auto it = loc_.find(p.dst);
  if (it == loc_.end()) return false;
  uint64_t age = p.hasDstPos ? ageDecode(p.dstAge) * 1000ull : UINT64_MAX;
  uint64_t pktObs = (age == UINT64_MAX || age > now) ? 0 : now - age;
  if (p.hasDstPos && it->second.obsMs <= pktObs) return false;
  p.hasDstPos = true;
  p.dstPos = it->second.pos;
  p.dstAge = ageEncode(static_cast<uint32_t>((now - std::min(now, it->second.obsMs)) / 1000));
  return true;
}

size_t Router::neighborCount(uint64_t now) const {
  size_t n = 0;
  for (auto& [id, nb] : nb_)
    if (now - nb.heardMs < kNbFreshMs) ++n;
  return n;
}

bool Router::closerNeighbor(const Position& tgt, uint64_t maxAge, uint64_t now) const {
  double mine = distanceM(pos_, tgt);
  for (auto& [id, n] : nb_)
    if (now - n.heardMs < maxAge && distanceM(n.pos, tgt) < mine - cfg_.progressThresholdM) return true;
  return false;
}

// -------------------------------------------------------------- forwarding --

void Router::geoSend(const Packet& f, uint64_t now, int tries, bool wide) {
  if (!havePos_ || !f.hasDstPos) {
    if (f.type == PType::Data) storeMsg(f, !f.hasDstPos, 1, now);
    return;
  }
  Key k = keyOf(f);
  if (tries < 0) {
    bool closer = closerNeighbor(f.dstPos, 10 * 60 * 1000, now) || nb_.count(f.dst);
    tries = closer ? 3 : 2;
  }
  Packet g = f;
  g.mode = Mode::Geo;
  g.hasFwdPos = true;
  g.fwdPos = pos_;
  g.prevHop = me16();
  g.nextHop = 0;
  // the narrow forwarding area only pays off where it is crowded (sim: DESIGN §5.2)
  g.wide = wide || neighborCount(now) < cfg_.areaMinNeighbors;
  TxPtr item = enqueue(g, f.src == cfg_.nodeId, now);
  uint32_t unit = airtimeMs(cfg_.phy, encodedSize(g));
  uint64_t wait = cfg_.contentionK * unit + 2 * unit + 2500;
  awaiting_[k] = Await{f, item, tries - 1, now + wait, pos_, f.prevHop, g.wide};
}

void Router::scheduleForward(const Packet& f, float rssi, uint64_t now) {
  if (!havePos_ || !f.hasFwdPos || !f.hasDstPos || f.hopsLeft == 0) return;
  const Position& tgt = f.dstPos;
  double dtot = distanceM(f.fwdPos, tgt);
  double prog = dtot - distanceM(pos_, tgt);
  if (prog < cfg_.progressThresholdM) return;
  if (!f.wide && dtot > 0) {
    // first attempt: only nodes inside a circle of diameter D toward the target
    // contend, so every candidate can hear every other one (no hidden forks)
    double d = std::min(cfg_.areaDiameterM, dtot);
    Position c = lerp(f.fwdPos, tgt, d / 2 / dtot);
    if (distanceM(pos_, c) > d / 2) return;
  }
  double q = clampd((rssi - cfg_.phy.sensitivityDbm) / 10.0, 0.2, 1.0);
  double s = 1.0 - clampd(prog * q / cfg_.phy.maxRangeM, 0, 1);
  bool destFirst = dtot < 0.4 * cfg_.phy.maxRangeM;   // let the target ack first
  double unit = airtimeMs(cfg_.phy, encodedSize(f));
  double delay = (s * cfg_.contentionK + (destFirst ? cfg_.contentionK + 1 : 0)) * unit;
  Packet g = f;
  g.hopsLeft = f.hopsLeft - 1;
  pending_[keyOf(f)] = Pending{g, now + static_cast<uint64_t>(delay) + rng_.range(0, kJitterMs), f.prevHop};
}

void Router::overheard(const Key& k, const Packet& f, uint64_t now) {
  if (f.hasFwdPos && f.hasDstPos) {
    const Position& tgt = f.dstPos;
    auto a = awaiting_.find(k);
    auto pd = pending_.find(k);
    auto st = store_.find(k);
    if (a != awaiting_.end()) {
      if (!a->second.item->sent) {
        if (f.prevHop != a->second.upstream) implicitAck(k);   // someone else took custody
      } else if (distanceM(f.fwdPos, tgt) <
                 distanceM(a->second.posAtTx, tgt) - cfg_.progressThresholdM / 2) {
        implicitAck(k);
      }
    } else if (pd != pending_.end()) {
      if (f.prevHop != pd->second.upstream) implicitAck(k);
    } else if (st != store_.end() && havePos_ && distanceM(f.fwdPos, tgt) < distanceM(pos_, tgt)) {
      store_.erase(st);
    }
  }
  // we already took custody and our upstream is retrying: tell it to stop
  Seen& s = seen_[k];
  if (s.custodyUp >= 0 && f.prevHop == s.custodyUp && !awaiting_.count(k) && !pending_.count(k) &&
      now - s.lackT > 5000) {
    s.lackT = now;
    sendLinkAck(k, f.prevHop, now);
  }
}

void Router::implicitAck(const Key& k) {
  auto a = awaiting_.find(k);
  if (a != awaiting_.end()) {
    a->second.item->cancelled = true;
    awaiting_.erase(a);
  }
  pending_.erase(k);
  store_.erase(k);
}

void Router::sendLinkAck(const Key& k, uint16_t to, uint64_t now) {
  Packet p;
  p.type = PType::LinkAck;
  p.mode = Mode::Unicast;
  p.src = cfg_.nodeId;
  p.dst = kBroadcast;
  p.msgId = k.mid;
  p.hopsLeft = 0;
  p.prevHop = me16();
  p.nextHop = to;
  p.payload.push_back(k.type);
  put32(p.payload, k.src);
  enqueue(p, false, now);
}

// -------------------------------------------------------------- DTN store --

void Router::storeMsg(const Packet& f, bool spray, int copies, uint64_t now) {
  Key k = keyOf(f);
  auto s = seen_.find(k);
  if (s != seen_.end() && s->second.acked) return;
  if (store_.size() >= cfg_.storeCapacity && !store_.count(k)) {
    auto oldest = std::min_element(store_.begin(), store_.end(), [](auto& a, auto& b) {
      if (a.second.pkt.prio != b.second.pkt.prio) return a.second.pkt.prio < b.second.pkt.prio;
      return a.second.created < b.second.created;
    });
    store_.erase(oldest);
  }
  Seen& sn = seen_[k];
  sn.t = now;
  store_[k] = StoreEntry{f, spray, copies, {}, now, now, sn.storeAttempts};
  ++stats_.stored;
}

bool Router::storeReady(const StoreEntry& e, uint64_t now) const {
  uint64_t gap = std::min<uint64_t>(600000, 30000ull << std::min<int>(e.attempts, 5));
  return now - e.last >= gap;
}

void Router::reattempt(const Key& k, uint64_t now) {
  StoreEntry e = store_[k];
  store_.erase(k);
  Seen& s = seen_[k];
  s.t = now;
  // the count survives a failed attempt (it lives on the dedup entry), so
  // backoff keeps growing if the message lands back in the store
  if (s.storeAttempts < 255) ++s.storeAttempts;
  geoSend(e.pkt, now, 2, true);
}

void Router::retryStore(uint64_t now) {
  if (!havePos_) return;
  std::vector<Key> keys;
  for (auto& [k, e] : store_) keys.push_back(k);
  for (const Key& k : keys) {
    auto it = store_.find(k);
    if (it == store_.end()) continue;
    StoreEntry& e = it->second;
    auto lc = loc_.find(e.pkt.dst);
    if (e.spray && lc != loc_.end()) {
      Packet p = e.pkt;
      store_.erase(it);
      fresherTarget(p, now);
      geoSend(p, now);
    } else if (!e.spray && e.pkt.hasDstPos && storeReady(e, now) &&
               closerNeighbor(e.pkt.dstPos, 180 * 1000, now)) {
      reattempt(k, now);
    }
  }
}

void Router::unicast(const Packet& f, uint16_t nh, int copies, uint64_t now) {
  Packet g = f;
  g.mode = Mode::Unicast;
  g.nextHop = nh;
  g.prevHop = me16();
  int log2 = 0;
  while ((1 << (log2 + 1)) <= copies && log2 < 15) ++log2;
  g.copiesLog2 = static_cast<uint8_t>(log2);
  g.hasFwdPos = havePos_;
  g.fwdPos = pos_;
  TxPtr item = enqueue(g, f.src == cfg_.nodeId, now);
  uni_[{keyOf(f), nh}] =
      UniAwait{f, item, 2, now + 3 * airtimeMs(cfg_.phy, encodedSize(g)) + 1500, copies};
}

// ---------------------------------------------------------------- receive --

void Router::onBeacon(const Packet& p, uint64_t now) {
  if (!p.hasFwdPos) return;
  auto old = nb_.find(p.src);
  bool isNew = old == nb_.end() || now - old->second.heardMs >= kNbFreshMs;
  const uint8_t* bloom = p.payload.size() >= kBeaconFixed ? p.payload.data() + kBloomOff : nullptr;
  // a known neighbor's Bloom filter still lacking an SOS we offered means the
  // offer was lost (hidden terminal): re-offer with backoff (DESIGN §8)
  bool lacks = true;
  if (isNew || bloom) lacks = offerSos(p.src, bloom, !isNew, now);
  if (!lacks) sosProbe_.erase(shortId(p.src));
  // a neighbor carrying SOS can only filter its offers by our Bloom filter:
  // let it see one now instead of at our next scheduled beacon, if it is new
  // or its filter holds something ours lacks (once per filter it shows). A
  // beacon with next_hop = us asks the same (an SOS holder checking an offer).
  uint64_t theirs = 0, answered = isNew ? 0 : old->second.bloomAnswered;
  if (bloom)
    for (int i = 0; i < 8; ++i) theirs |= static_cast<uint64_t>(bloom[i]) << (8 * i);
  bool carrier = !p.payload.empty() && (p.payload[0] & kBeaconSos);
  bool ahead = bloom && (theirs & ~ownBloom()) && theirs != answered;
  if ((carrier && (isNew || ahead)) || (p.nextHop && p.nextHop == me16())) {
    beaconSoonAt_ = now + rng_.range(200, 2000);
    answered = theirs;
  }
  nb_[p.src] = {p.fwdPos, now, answered};
  learn(p.src, p.fwdPos, now);
  wantIdent(p.src, now);
  if (p.payload.size() >= kBeaconFixed) {
    for (size_t off = kBeaconFixed; off + kGossipSize <= p.payload.size(); off += kGossipSize) {
      const uint8_t* g = p.payload.data() + off;
      uint8_t age = g[10];
      if (age == 0xFF) continue;
      uint64_t a = ageDecode(age) * 1000ull;
      learn(get32(g), unpackPos(g + 4), now > a ? now - a : 0);
    }
  }
  if (!havePos_) return;
  std::vector<Key> keys;
  for (auto& [k, e] : store_) keys.push_back(k);
  for (const Key& k : keys) {
    auto it = store_.find(k);
    if (it == store_.end()) continue;
    StoreEntry& e = it->second;
    if (e.pkt.dst == p.src) {
      unicast(e.pkt, shortId(p.src), e.copies, now);
      continue;
    }
    if (e.spray && loc_.count(e.pkt.dst)) {
      Packet q = e.pkt;
      store_.erase(it);
      fresherTarget(q, now);
      geoSend(q, now);
      continue;
    }
    if (!e.spray) {
      double myDist = distanceM(pos_, e.pkt.dstPos);
      if (storeReady(e, now) &&
          distanceM(p.fwdPos, e.pkt.dstPos) < myDist - cfg_.progressThresholdM)
        reattempt(k, now);
    } else if (e.copies > 1 && !e.given.count(shortId(p.src))) {
      e.given.insert(shortId(p.src));
      unicast(e.pkt, shortId(p.src), e.copies / 2, now);
    }
  }
}

void Router::onLinkAck(const Packet& p, uint64_t now) {
  (void)now;
  if (p.payload.size() < 5) return;
  Key k{p.payload[0], get32(p.payload.data() + 1), p.msgId};
  if (p.nextHop != me16()) {
    if (pending_.count(k)) implicitAck(k);   // it already arrived further on
    return;
  }
  auto u = uni_.find({k, p.prevHop});
  if (u == uni_.end()) {
    implicitAck(k);
    return;
  }
  bool toDst = shortId(u->second.orig.dst) == p.prevHop;
  int given = u->second.copies;
  uni_.erase(u);
  auto se = store_.find(k);
  if (toDst) {
    if (se != store_.end()) store_.erase(se);
    seen_[k].acked = true;
  } else if (se != store_.end()) {
    se->second.copies -= given;
    if (se->second.copies < 1) store_.erase(se);
  }
}

void Router::onAck(const Packet& p, uint64_t now) {
  Key dkey{static_cast<uint8_t>(PType::Data), p.dst, p.msgId};
  Seen& ds = seen_[dkey];
  bool first = !ds.acked;
  ds.acked = true;
  ds.t = now;
  implicitAck(dkey);
  Key akey = keyOf(p);
  if (p.dst == cfg_.nodeId) {
    if (first) app_.onDelivered(p.src, p.msgId);
    Seen& as = seen_[akey];
    as.t = now;
    if (now - as.lackT > 5000) {
      as.lackT = now;
      sendLinkAck(akey, p.prevHop, now);
    }
    return;
  }
  auto s = seen_.find(akey);
  if (s != seen_.end()) {
    overheard(akey, p, now);
    return;
  }
  seen_[akey].t = now;
  // ack's rssi is not tracked separately: treat as a solid link
  scheduleForward(p, cfg_.phy.sensitivityDbm + 10, now);
}

void Router::onData(const Packet& p, float rssi, uint64_t now) {
  Key k = keyOf(p);
  if (p.dst == cfg_.nodeId) {
    std::vector<uint8_t> plain;
    if (p.encrypted ? !openData(p, plain) : cfg_.requireEncryption) {
      ++stats_.rxAuthFail;   // no seen_ entry: a forgery must not block the real frame
      return;
    }
    Seen& s = seen_[k];
    s.t = now;
    if (!s.delivered) {
      s.delivered = true;
      ++stats_.delivered;
      app_.onMessage(p.src, p.msgId, p.encrypted ? plain : p.payload);
    }
    if (p.mode == Mode::Unicast) sendLinkAck(k, p.prevHop, now);
    if (now - s.ackSentT > 10000 || s.ackSentT == 0) {
      s.ackSentT = now;
      Packet a;
      a.type = PType::Ack;
      a.src = cfg_.nodeId;
      a.dst = p.src;
      a.msgId = p.msgId;
      a.hopsLeft = cfg_.hopLimit;
      if (p.hasSrcPos) {
        a.hasDstPos = true;
        a.dstPos = p.srcPos;
        a.dstAge = p.srcAge;
      }
      fresherTarget(a, now);
      seen_[keyOf(a)].t = now;
      if (a.hasDstPos && havePos_)
        geoSend(a, now, 2);
      else if (p.mode != Mode::Unicast)
        sendLinkAck(k, p.prevHop, now);   // no way to route an ack: at least stop the last hop
    }
    return;
  }
  auto s = seen_.find(k);
  if (s != seen_.end() && s->second.acked) {
    if (p.mode == Mode::Unicast && p.nextHop == me16()) sendLinkAck(k, p.prevHop, now);
    return;
  }
  if (p.mode == Mode::Unicast) {
    if (p.nextHop != me16()) return;
    sendLinkAck(k, p.prevHop, now);
    if (store_.count(k) || awaiting_.count(k)) return;
    seen_[k].t = now;
    Packet g = p;
    if (havePos_ && fresherTarget(g, now)) {
      geoSend(g, now);
    } else if (havePos_ && g.hasDstPos) {
      geoSend(g, now);
    } else {
      storeMsg(g, true, 1 << p.copiesLog2, now);
    }
    return;
  }
  if (s != seen_.end()) {
    overheard(k, p, now);
    return;
  }
  seen_[k].t = now;
  Packet g = p;
  fresherTarget(g, now);
  scheduleForward(g, rssi, now);
}

// ----------------------------------------------------------------- beacons --

void Router::beaconTick(uint64_t now) {
  if (now > utilT_) util_ = double(heardAirMs_ - heardMark_) / double(now - utilT_);
  heardMark_ = heardAirMs_;
  utilT_ = now;
  if (havePos_) {
    double scale = std::max(1.0, util_ / cfg_.utilTarget);
    uint64_t el = now - lastBeaconT_;
    double moved = beaconed_ ? distanceM(pos_, lastBeaconPos_) : 1e9;
    if (!beaconed_ || (el >= cfg_.beaconMinMs * scale &&
                       (moved >= cfg_.beaconMoveM || el >= cfg_.beaconMaxMs * scale))) {
      sendBeacon(now);
    }
  }
  retryStore(now);
}

// Our beacon's Bloom filter, byte i in bits 8i..8i+7.
uint64_t Router::ownBloom() const {
  uint8_t bloom[8] = {0};
  for (auto& [k, e] : store_) bloomAdd(bloom, k.src, k.mid);
  for (auto& [src, e] : sos_) bloomAdd(bloom, src, e.pkt.msgId);
  uint64_t b = 0;
  for (int i = 0; i < 8; ++i) b |= static_cast<uint64_t>(bloom[i]) << (8 * i);
  return b;
}

void Router::sendBeacon(uint64_t now, uint16_t to) {
  Packet b;
  b.type = PType::Beacon;
  b.mode = Mode::Unicast;   // 1-hop, never forwarded
  b.src = cfg_.nodeId;
  b.dst = kBroadcast;
  b.msgId = rng_.next();
  b.hopsLeft = 0;
  b.prevHop = me16();
  b.nextHop = to;           // non-zero: that neighbor should answer with its beacon
  b.hasFwdPos = true;
  b.fwdPos = pos_;
  auto& pl = b.payload;
  pl.push_back(kBeaconHasGps | (sos_.empty() ? 0 : kBeaconSos));
  pl.push_back(0);
  pl.push_back(0);                                      // velocity: TODO from GPS RMC
  pl.push_back(static_cast<uint8_t>(clampd(util_, 0, 1) * 255));
  pl.push_back(static_cast<uint8_t>(std::min<size_t>(store_.size() + sos_.size(), 255)));
  uint64_t bloom = ownBloom();
  for (int i = 0; i < 8; ++i) pl.push_back(static_cast<uint8_t>(bloom >> (8 * i)));
  std::vector<std::pair<uint32_t, LocEntry>> fresh;
  for (auto& [id, l] : loc_)
    if (now - l.obsMs < 30 * 60 * 1000) fresh.push_back({id, l});
  for (size_t i = 0; i < kGossipEntries && !fresh.empty(); ++i) {
    size_t j = rng_.range(0, static_cast<uint32_t>(fresh.size()));
    put32(pl, fresh[j].first);
    uint8_t pp[kPosSize];
    packPos(fresh[j].second.pos, pp);
    pl.insert(pl.end(), pp, pp + kPosSize);
    pl.push_back(ageEncode(static_cast<uint32_t>((now - fresh[j].second.obsMs) / 1000)));
    fresh.erase(fresh.begin() + j);
  }
  enqueue(b, true, now);
  lastBeaconT_ = now;
  lastBeaconPos_ = pos_;
  beaconed_ = true;
}

// --------------------------------------------------------------------- SOS --

bool Router::startSos(const std::string& text, uint8_t battery, uint8_t accuracy, uint64_t now) {
  if (sosEver_ && now - sosLastTx_ < cfg_.sosMinGapMs) return false;
  sosText_ = text.substr(0, kSosTextMax);
  sosBattery_ = battery;
  sosAccuracy_ = accuracy;
  sosOn_ = true;
  sendSosNow(now);
  return true;
}

void Router::sendSosNow(uint64_t now) {
  Packet p;
  p.type = PType::Sos;
  p.prio = kPrioSos;
  p.mode = Mode::ScopedFlood;
  p.src = cfg_.nodeId;
  p.dst = kBroadcast;
  p.msgId = rng_.next() | 1;      // fresh per seq: the Bloom filter tells seqs apart
  p.hopsLeft = cfg_.hopLimit;
  p.prevHop = me16();
  if (havePos_) {
    p.hasSrcPos = true;
    p.srcPos = pos_;
    p.srcAge = 0;
  }
  ++sosSeq_;
  p.payload = {static_cast<uint8_t>(sosSeq_ & 0xFF), static_cast<uint8_t>(sosSeq_ >> 8), sosAccuracy_,
               sosBattery_};
  p.payload.insert(p.payload.end(), sosText_.begin(), sosText_.end());
  sosEver_ = true;
  sosLastTx_ = now;
  // our own SOS sits in the store too, so it is offered to new neighbors
  SosEntry& e = sos_[cfg_.nodeId];
  if (e.offer) e.offer->cancelled = true;
  auto offered = std::move(e.offered);
  e = SosEntry{p, sosSeq_, now, now, nullptr, 0, nullptr, std::move(offered)};
  enqueue(p, true, now);   // prio 2: jumps the queue, bypasses the fair-share budget
  ++stats_.sosTx;
}

bool Router::hasSos(uint32_t src, uint16_t* seq) const {
  auto it = sos_.find(src);
  if (it == sos_.end()) return false;
  if (seq) *seq = it->second.seq;
  return true;
}

void Router::onSos(const Packet& p, uint64_t now) {
  if (p.src == cfg_.nodeId || p.payload.size() < kSosHeader || p.encrypted) return;
  uint16_t seq = static_cast<uint16_t>(p.payload[0] | (p.payload[1] << 8));
  auto it = sos_.find(p.src);
  if (it != sos_.end() && !seqNewer(seq, it->second.seq)) {
    SosEntry& e = it->second;
    if (seq == e.seq) {
      if (e.flood && !e.flood->sent && ++e.dups >= kSosDupCancel) e.flood->cancelled = true;
      if (e.offer && !e.offer->sent) e.offer->cancelled = true;   // someone else offered it
      if (p.prevHop) sosProbe_.erase(p.prevHop);   // its sender has it: no need to check
    } else {
      offerSos(0, nullptr, false, now);   // sender is behind: our newer seq goes out (rate-limited)
    }
    return;
  }
  if (it == sos_.end() && sos_.size() >= cfg_.sosCapacity) {
    auto oldest = std::min_element(sos_.begin(), sos_.end(), [](auto& a, auto& b) {
      return a.second.heardMs < b.second.heardMs;
    });
    sos_.erase(oldest);
  }
  bool fresh = it == sos_.end();
  SosEntry& e = sos_[p.src];
  if (e.flood) e.flood->cancelled = true;
  if (e.offer) e.offer->cancelled = true;
  // the first seq floods at once; superseding seqs (every 5 min) are stored
  // everywhere but re-flooded by relays at most every kSosRelayGapMs (sim: the
  // full-rate re-flood tripled SOS airtime without improving reach)
  bool relay = fresh || now - e.relayMs >= kSosRelayGapMs;
  uint64_t relayMs = relay ? now : e.relayMs;
  auto offered = std::move(e.offered);
  e = SosEntry{p, seq, now, relayMs, nullptr, 0, nullptr, std::move(offered)};
  ++stats_.sosHeard;
  if (p.hopsLeft > 0 && relay) {
    Packet g = p;
    g.hopsLeft = p.hopsLeft - 1;
    g.prevHop = me16();
    g.nextHop = 0;
    e.flood = enqueue(g, false, now);
    uint32_t unit = airtimeMs(cfg_.phy, encodedSize(g));
    e.flood->readyAt = now + rng_.range(0, 2 * unit + kJitterMs);
  }
  SosInfo info;
  info.src = p.src;
  info.seq = seq;
  info.pos = p.hasSrcPos ? p.srcPos : Position{};
  info.accuracy = p.payload[2];
  info.battery = p.payload[3];
  info.text.assign(p.payload.begin() + kSosHeader, p.payload.end());
  app_.onSos(info);
  if (cfg_.responder) ackSos(p.src, now);
}

bool Router::ackSos(uint32_t sosSrc, uint64_t now) {
  auto it = sos_.find(sosSrc);
  if (it == sos_.end() || sosSrc == cfg_.nodeId) return false;
  Packet a;
  a.type = PType::SosAck;
  a.prio = kPrioSos;
  a.mode = Mode::ScopedFlood;
  a.src = cfg_.nodeId;
  a.dst = sosSrc;
  a.msgId = rng_.next() | 1;
  a.hopsLeft = cfg_.hopLimit;
  a.prevHop = me16();
  a.payload = {static_cast<uint8_t>(it->second.seq & 0xFF), static_cast<uint8_t>(it->second.seq >> 8)};
  seen_[keyOf(a)].t = now;
  enqueue(a, true, now);
  return true;
}

void Router::onSosAck(const Packet& p, uint64_t now) {
  if (p.payload.size() < 2) return;
  Key k = keyOf(p);
  if (seen_.count(k)) return;
  seen_[k].t = now;
  if (p.dst == cfg_.nodeId) {
    app_.onSosAck(p.src, static_cast<uint16_t>(p.payload[0] | (p.payload[1] << 8)));
    return;
  }
  if (p.hopsLeft == 0) return;
  Packet g = p;
  g.hopsLeft = p.hopsLeft - 1;
  g.prevHop = me16();
  TxPtr t = enqueue(g, false, now);
  t->readyAt = now + rng_.range(0, 2 * airtimeMs(cfg_.phy, encodedSize(g)) + kJitterMs);
}

// Offer every SOS we hold that `bloom` (a neighbor's beacon filter) lacks.
// known = false: a new neighbor, offered at most every kSosReofferMs. known =
// true: one we heard within kNbFreshMs, so an earlier offer was probably lost;
// re-offered after sosRetryGap (and not within kSosRetryMs of hearing the seq,
// while the flood may still reach it). Each offer, or offer held back, schedules
// a probe (a beacon with next_hop = it) so that its Bloom filter comes back
// without waiting up to 15 min for its next beacon. Returns false if it lacks
// nothing we hold.
// nbId = 0 / bloom = null: offer to whoever is listening (a neighbor is behind).
bool Router::offerSos(uint32_t nbId, const uint8_t* bloom, bool known, uint64_t now) {
  uint16_t nb16 = shortId(nbId);
  bool lacks = false;
  for (auto& [src, e] : sos_) {
    if (src == nbId) continue;                                   // its own SOS
    auto o = e.offered.find(nb16);
    if (bloom && bloomHas(bloom, src, e.pkt.msgId)) {           // already carries this seq
      if (o != e.offered.end()) o->second.tries = 0;
      continue;
    }
    lacks = true;
    uint8_t tries = 0;
    uint64_t readyAt = now;
    if (o != e.offered.end()) {
      readyAt = o->second.t + (known ? sosRetryGap(o->second.tries) : kSosReofferMs);
      if (known) tries = static_cast<uint8_t>(std::min<int>(o->second.tries + 1, 255));
    }
    if (known) readyAt = std::max(readyAt, e.heardMs + kSosRetryMs);   // our own flood may still reach it
    if (now < readyAt) {
      // held back: make sure its Bloom filter comes back once we may offer
      if (known && tries <= kSosRetryMax) probeSos(nb16, readyAt, kSosRetryMs / 4);
      continue;
    }
    if (e.offered.size() >= kOfferedCapacity && o == e.offered.end()) {
      auto oldest = std::min_element(e.offered.begin(), e.offered.end(),
                                     [](auto& a, auto& b) { return a.second.t < b.second.t; });
      e.offered.erase(oldest);
    }
    e.offered[nb16] = {now, tries};
    if (nbId && tries < kSosRetryMax) probeSos(nb16, now + sosRetryGap(tries), sosRetryGap(tries) / 4);
    if (e.offer && !e.offer->sent && !e.offer->cancelled) continue;   // one queued copy serves all
    // an offer starts a fresh scoped flood in the neighbor's partition; nodes
    // that already hold this seq drop it, so it only spreads where it is new
    Packet g = e.pkt;
    g.hopsLeft = cfg_.hopLimit;
    g.prevHop = me16();
    g.nextHop = nb16;
    // jittered: when several holders meet the same newcomer, one offer suffices
    e.offer = enqueue(g, false, now);
    e.offer->readyAt = now + rng_.range(0, 2 * airtimeMs(cfg_.phy, encodedSize(g)) + kJitterMs);
    ++stats_.sosTx;
  }
  return lacks;
}

// Schedules a beacon with next_hop = nb16 at `at` plus up to `jitterMs`, unless
// one is already due by then.
void Router::probeSos(uint16_t nb16, uint64_t at, uint64_t jitterMs) {
  auto it = sosProbe_.find(nb16);
  if (it != sosProbe_.end() ? it->second >= at : sosProbe_.size() >= kSosProbeCapacity) return;
  sosProbe_[nb16] = at + rng_.range(0, static_cast<uint32_t>(jitterMs));
}

void Router::sosTick(uint64_t now) {
  if (sosOn_ && now - sosLastTx_ >= cfg_.sosRepeatMs) sendSosNow(now);
  if (sos_.empty()) sosProbe_.clear();
  // one probe per beacon gap; the others wait their turn
  for (auto it = sosProbe_.begin(); it != sosProbe_.end(); ++it) {
    if (now < it->second) continue;
    if (havePos_ && (!beaconed_ || now - lastBeaconT_ >= kBeaconReplyMinMs)) {
      uint16_t to = it->first;
      sosProbe_.erase(it);
      sendBeacon(now, to);
    }
    break;
  }
  for (auto it = sos_.begin(); it != sos_.end();) {
    bool own = it->first == cfg_.nodeId && sosOn_;
    if (!own && now - it->second.heardMs > cfg_.sosTtlMs) {
      if (it->second.flood) it->second.flood->cancelled = true;
      if (it->second.offer) it->second.offer->cancelled = true;
      it = sos_.erase(it);
    } else {
      ++it;
    }
  }
}

// ------------------------------------------------------------------- IDENT --

bool Router::requestIdent(uint32_t id, uint64_t now) {
  if (!haveSigKey_ || id == cfg_.nodeId || id == kBroadcast) return false;
  askIdent(id, now);
  return true;
}

void Router::askIdent(uint32_t id, uint64_t now) {
  if (identAsked_.size() >= kIdentAskedCapacity && !identAsked_.count(id)) {
    auto oldest = std::min_element(identAsked_.begin(), identAsked_.end(),
                                   [](auto& a, auto& b) { return a.second < b.second; });
    identAsked_.erase(oldest);
  }
  identAsked_[id] = now;
  sendIdent(id, now);
}

// A neighbor we hold no key for: ask for its IDENT after a short jitter. If
// its broadcast (answering someone else) arrives first, the ask is dropped.
void Router::wantIdent(uint32_t id, uint64_t now) {
  if (!haveSigKey_ || peers_.count(id) || identWant_.count(id)) return;
  if (identWant_.size() >= kIdentWantCapacity) return;
  auto a = identAsked_.find(id);
  if (a != identAsked_.end() && now - a->second < kIdentAskGapMs) return;
  identWant_[id] = now + rng_.range(500, 5000);
}

// dst = broadcast announces us; dst = a node asks it for its IDENT (and
// announces us to everyone in range, since the body is the same).
void Router::sendIdent(uint32_t dst, uint64_t now) {
  Packet p;
  p.type = PType::Ident;
  p.mode = Mode::Unicast;   // 1-hop, never forwarded
  p.src = cfg_.nodeId;
  p.dst = dst;
  p.msgId = rng_.next() | 1;
  p.hopsLeft = 0;
  p.prevHop = me16();
  p.nextHop = dst == kBroadcast ? 0 : shortId(dst);
  p.payload = identBody_;
  enqueue(p, true, now);
  lastIdentT_ = now;
  identSent_ = true;
  ++stats_.identTx;
}

void Router::onIdent(const Packet& p, uint64_t now) {
  if (p.src == cfg_.nodeId) return;
  uint8_t edPub[32];
  std::string name;
  if (p.encrypted || !identCheck(p.payload, p.src, edPub, name)) {
    ++stats_.identBad;   // forged signature or a key that does not hash to src
    return;
  }
  ++stats_.identRx;
  if (p.dst == cfg_.nodeId && haveSigKey_ && !identReplyAt_) {
    // answer with a broadcast: every neighbor that lacks our key learns it too
    uint64_t at = now + rng_.range(100, 1000);
    if (identSent_) at = std::max(at, lastIdentT_ + kIdentReplyGapMs);
    identReplyAt_ = at;
  }
  uint8_t x[32];
  ed25519ToX25519(edPub, x);
  auto it = peers_.find(p.src);
  if (it == peers_.end()) {
    if (storePeer(p.src, x, false, now)) {   // trust on first use
      peers_[p.src].name = name;
      app_.onPeerIdent(p.src, name, true);
    }
  } else if (std::memcmp(it->second.xPub.data(), x, 32) != 0) {
    ++stats_.keyChanges;   // keep the key we have; the host decides
    app_.onKeyChange(p.src, edPub, name);
  } else if (it->second.name != name) {
    it->second.name = name;   // signed by the key we already trust
    app_.onPeerIdent(p.src, name, false);
  }
}

void Router::identTick(uint64_t now) {
  if (!haveSigKey_) return;
  for (auto it = identWant_.begin(); it != identWant_.end();) {
    if (it->second > now) {
      ++it;
      continue;
    }
    uint32_t id = it->first;
    it = identWant_.erase(it);
    if (!peers_.count(id)) askIdent(id, now);
  }
  if (identReplyAt_ && now >= identReplyAt_) {
    identReplyAt_ = 0;
    sendIdent(kBroadcast, now);
  }
  if (cfg_.identIntervalMs == 0) return;
  if (nextIdentAt_ == 0) {
    nextIdentAt_ = now + rng_.range(1000, 15000);   // announce soon after start
  } else if (now >= nextIdentAt_) {
    if (now - lastIdentT_ >= kIdentReplyGapMs || !identSent_) sendIdent(kBroadcast, now);
    double scale = std::max(1.0, util_ / cfg_.utilTarget);
    nextIdentAt_ = now + static_cast<uint64_t>(cfg_.identIntervalMs * scale) +
                   rng_.range(0, cfg_.identIntervalMs / 10 + 1);
  }
}

}  // namespace gw
