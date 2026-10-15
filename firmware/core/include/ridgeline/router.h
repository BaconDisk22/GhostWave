// Ridgeline router: contention-based geographic forwarding, DTN custody store,
// spray-and-wait, airtime budget and CSMA (docs/DESIGN.md §5–6), the SOS lane
// (§8) and DATA encryption (§9.1).
//
// Platform-agnostic: the host supplies a RadioPort (send / CAD), an AppPort
// (delivery callbacks), a position, and calls tick() every few milliseconds.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ridgeline/packet.h"
#include "ridgeline/util.h"

namespace rl {

class RadioPort {
 public:
  virtual ~RadioPort() = default;
  // True if a LoRa preamble/packet is on the air right now (CAD or RSSI).
  virtual bool channelBusy() = 0;
  // Transmit one frame; returns when it is on the air or failed.
  virtual bool transmit(const uint8_t* buf, size_t len) = 0;
};

// SOS body (DESIGN §8). Position travels in the frame's src_pos.
struct SosInfo {
  uint32_t src = 0;
  uint16_t seq = 0;
  Position pos;               // valid = false if the originator had no fix
  uint8_t accuracy = 0xFF;    // 4 m steps, 0xFF unknown
  uint8_t battery = 0xFF;     // percent, 0xFF unknown
  std::string text;           // <= kSosTextMax bytes
};
constexpr size_t kSosTextMax = 60;

class AppPort {
 public:
  virtual ~AppPort() = default;
  virtual void onMessage(uint32_t src, uint32_t msgId, const std::vector<uint8_t>& payload) = 0;
  // End-to-end receipt for a message we sent.
  virtual void onDelivered(uint32_t dst, uint32_t msgId) { (void)dst, (void)msgId; }
  // A new SOS, or a newer seq of one we already hold.
  virtual void onSos(const SosInfo& sos) { (void)sos; }
  // A responder acknowledged our SOS.
  virtual void onSosAck(uint32_t responder, uint16_t seq) { (void)responder, (void)seq; }
};

struct Config {
  uint32_t nodeId = 0;
  Phy phy;
  double progressThresholdM = 150;
  double areaDiameterM = 2500;      // first-attempt forwarding area
  size_t areaMinNeighbors = 10;     // ...used only when this crowded
  int contentionK = 3;
  uint8_t hopLimit = 15;
  uint8_t sprayCopiesLog2 = 3;      // 8 copies
  size_t storeCapacity = 64;
  uint64_t storeTtlMs = 6ull * 3600 * 1000;
  double beaconMoveM = 250;
  uint32_t beaconMaxMs = 15 * 60 * 1000;
  uint32_t beaconMinMs = 60 * 1000;
  double utilTarget = 0.25;
  double budgetShare = 0.30;        // airtime fair share for originated traffic
  double budgetFloor = 0.02;
  double budgetDepthMs = 8000;
  uint64_t rngSeed = 0;
  bool requireEncryption = false;   // drop plaintext DATA addressed to us
  bool responder = false;           // auto-reply SOS_ACK to every new SOS seq
  uint64_t sosTtlMs = 24ull * 3600 * 1000;
  uint32_t sosRepeatMs = 5 * 60 * 1000;
  uint32_t sosMinGapMs = 60 * 1000;  // originate at most 1 SOS per this
  size_t sosCapacity = 16;           // distinct originators held
  size_t seenCapacity = 0;           // duplicate-suppression entries; 0 = unbounded (MCU ports set it)
};

struct Key {
  uint8_t type;
  uint32_t src;
  uint32_t mid;
  bool operator<(const Key& o) const {
    if (type != o.type) return type < o.type;
    if (src != o.src) return src < o.src;
    return mid < o.mid;
  }
  bool operator==(const Key& o) const { return type == o.type && src == o.src && mid == o.mid; }
};

struct RouterStats {
  uint32_t tx = 0, rx = 0, rxBad = 0, delivered = 0, forwarded = 0, stored = 0;
  uint32_t rxAuthFail = 0, sosHeard = 0, sosTx = 0;
  uint64_t airMs = 0;
};

class Router {
 public:
  Router(const Config& cfg, RadioPort& radio, AppPort& app);
  ~Router();
  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  void setPosition(const Position& p, uint64_t nowMs);
  // Seed the location table (contact card / QR code / manual entry).
  void addKnownPosition(uint32_t id, const Position& p, uint64_t observedMs);
  // Keys (DESIGN §9.1). DATA to a peer with a known key is sealed with the DM
  // key, else with the channel key if set, else sent with the CRC MIC only.
  void setIdentityKey(const uint8_t x25519Secret[32]);
  void setChannelKey(const uint8_t key[32]);
  // Call after setIdentityKey. False on a low-order key or a full table.
  bool addPeerKey(uint32_t id, const uint8_t x25519Public[32]);

  // Queue a message; returns its msg_id, or 0 if it cannot be sealed (too big).
  uint32_t send(uint32_t dst, const std::vector<uint8_t>& payload, uint64_t nowMs);

  // SOS lane (DESIGN §8). Sends now and re-sends every sosRepeatMs with the
  // current position and a new seq. Calling again updates the body. Returns
  // false when rate limited (sosMinGapMs since the last origination).
  bool startSos(const std::string& text, uint8_t batteryPct, uint8_t accuracy4m, uint64_t nowMs);
  void stopSos() { sosOn_ = false; }
  bool sosActive() const { return sosOn_; }
  // Send SOS_ACK for the SOS we hold from `sosSrc`. False if we hold none.
  bool ackSos(uint32_t sosSrc, uint64_t nowMs);
  size_t sosCount() const { return sos_.size(); }
  // True if we hold an SOS from `src`; fills its seq.
  bool hasSos(uint32_t src, uint16_t* seq = nullptr) const;

  void onReceive(const uint8_t* buf, size_t len, float rssiDbm, uint64_t nowMs);
  void tick(uint64_t nowMs);

  const RouterStats& stats() const { return stats_; }
  size_t storeSize() const { return store_.size(); }
  size_t neighborCount(uint64_t nowMs) const;
  size_t neighborTableSize() const { return nb_.size(); }
  size_t seenSize() const { return seen_.size(); }
  uint32_t id() const { return cfg_.nodeId; }

 private:
  struct TxItem {
    Packet pkt;
    bool own;
    uint64_t readyAt;
    uint64_t seq;
    bool cancelled = false;
    bool sent = false;
  };
  using TxPtr = std::shared_ptr<TxItem>;
  struct LocEntry { Position pos; uint64_t obsMs; };
  struct Neighbor { Position pos; uint64_t heardMs; };
  struct Seen {
    uint64_t t = 0;
    int32_t custodyUp = -1;     // short id we took custody from
    uint64_t lackT = 0;
    bool acked = false;
    bool delivered = false;
    uint64_t ackSentT = 0;
    uint8_t storeAttempts = 0;
  };
  struct Pending { Packet pkt; uint64_t fireAt; uint16_t upstream; };
  struct Await {
    Packet orig;
    TxPtr item;
    int triesLeft;
    uint64_t deadline;
    Position posAtTx;
    uint16_t upstream;
    bool wide;
  };
  struct StoreEntry {
    Packet pkt;
    bool spray;
    int copies;
    std::set<uint16_t> given;
    uint64_t last;
    uint64_t created;
    uint8_t attempts;
  };
  struct UniAwait { Packet orig; TxPtr item; int triesLeft; uint64_t deadline; int copies; };
  struct SosEntry {
    Packet pkt;                 // as heard (or as sent, for our own)
    uint16_t seq;
    uint64_t heardMs;
    uint64_t relayMs;           // last time we re-flooded a seq from this originator
    TxPtr flood;                // pending scoped-flood rebroadcast
    uint8_t dups = 0;           // copies heard while that is pending
    TxPtr offer;                // pending offer to a new neighbor
    std::map<uint16_t, uint64_t> offered;   // neighbor -> last offer time
  };

  static Key keyOf(const Packet& p) { return {static_cast<uint8_t>(p.type), p.src, p.msgId}; }
  uint16_t me16() const { return shortId(cfg_.nodeId); }

  TxPtr enqueue(const Packet& p, bool own, uint64_t now);
  void serviceMac(uint64_t now);
  double budgetRate(uint64_t now) const;

  void learn(uint32_t id, const Position& p, uint64_t obsMs);
  bool fresherTarget(Packet& p, uint64_t now) const;
  void geoSend(const Packet& f, uint64_t now, int tries = -1, bool wide = false);
  void scheduleForward(const Packet& f, float rssi, uint64_t now);
  void overheard(const Key& k, const Packet& f, uint64_t now);
  void implicitAck(const Key& k);
  void sendLinkAck(const Key& k, uint16_t to, uint64_t now);
  void storeMsg(const Packet& f, bool spray, int copies, uint64_t now);
  void unicast(const Packet& f, uint16_t nh, int copies, uint64_t now);
  void onBeacon(const Packet& p, uint64_t now);
  void onLinkAck(const Packet& p, uint64_t now);
  void onAck(const Packet& p, uint64_t now);
  void onData(const Packet& p, float rssi, uint64_t now);
  void beaconTick(uint64_t now);
  void retryStore(uint64_t now);
  bool storeReady(const StoreEntry& e, uint64_t now) const;
  void reattempt(const Key& k, uint64_t now);
  bool closerNeighbor(const Position& tgt, uint64_t maxAgeMs, uint64_t now) const;
  void sendBeacon(uint64_t now);
  bool openData(const Packet& p, std::vector<uint8_t>& plain);
  void sendSosNow(uint64_t now);
  void onSos(const Packet& p, uint64_t now);
  void onSosAck(const Packet& p, uint64_t now);
  void offerSos(uint32_t nbId, const uint8_t* bloom, uint64_t now);
  void sosTick(uint64_t now);

  Config cfg_;
  RadioPort& radio_;
  AppPort& app_;
  Rng rng_;
  RouterStats stats_;

  bool havePos_ = false;
  Position pos_;

  std::vector<TxPtr> txq_;
  uint64_t txSeq_ = 0;
  uint64_t macBackoffUntil_ = 0;
  double tokensMs_;
  uint64_t tokensT_ = 0;

  std::map<uint32_t, LocEntry> loc_;
  std::map<uint32_t, Neighbor> nb_;
  std::map<Key, Seen> seen_;
  std::map<Key, Pending> pending_;
  std::map<Key, Await> awaiting_;
  std::map<Key, StoreEntry> store_;
  std::map<std::pair<Key, uint16_t>, UniAwait> uni_;

  uint64_t nextBeaconCheck_ = 0;
  uint64_t lastBeaconT_ = 0;
  bool beaconed_ = false;
  Position lastBeaconPos_;
  uint64_t heardAirMs_ = 0, heardMark_ = 0, utilT_ = 0;
  double util_ = 0;
  uint64_t beaconSoonAt_ = 0;   // early beacon so an SOS carrier sees our Bloom filter

  bool haveIdKey_ = false, haveChanKey_ = false;
  std::array<uint8_t, 32> idKey_{}, chanKey_{};
  std::map<uint32_t, std::array<uint8_t, 32>> dmKeys_;   // peer -> derived DM key

  std::map<uint32_t, SosEntry> sos_;   // by originator; newest seq only
  bool sosOn_ = false, sosEver_ = false;
  uint16_t sosSeq_ = 0;
  uint64_t sosLastTx_ = 0;
  std::string sosText_;
  uint8_t sosBattery_ = 0xFF, sosAccuracy_ = 0xFF;
};

}  // namespace rl
