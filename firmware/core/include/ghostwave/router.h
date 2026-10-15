// GhostWave router: contention-based geographic forwarding, DTN custody store,
// spray-and-wait, airtime budget and CSMA (docs/DESIGN.md §5–6), the SOS lane
// (§8), DATA encryption (§9.1) and IDENT key exchange (§4.5).
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

#include "ghostwave/packet.h"
#include "ghostwave/util.h"

namespace gw {

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
  // A verified IDENT taught us a new peer key (trust on first use), or a known
  // peer changed its name.
  virtual void onPeerIdent(uint32_t id, const std::string& name, bool firstSeen) {
    (void)id, (void)name, (void)firstSeen;
  }
  // A correctly signed IDENT for `id` carries a different key from the one we
  // hold. Since node ID = hash(key), an honest node cannot do this: it means a
  // brute-forced ID collision (DESIGN §9.2). The old key stays in use; call
  // Router::addPeerKey(id, ed25519ToX25519(newEdPub)) to accept the new one.
  virtual void onKeyChange(uint32_t id, const uint8_t newEdPub[32], const std::string& name) {
    (void)id, (void)newEdPub, (void)name;
  }
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
  uint32_t identIntervalMs = 60 * 60 * 1000;   // broadcast IDENT this often; 0 = only on request
  size_t peerCapacity = 64;          // peer keys held (pinned + learned)
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
  uint32_t identTx = 0, identRx = 0, identBad = 0, keyChanges = 0;
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
  // setSigningKey sets our Ed25519 identity: it derives the X25519 secret (no
  // separate setIdentityKey needed) and enables IDENT. False if the key's node
  // ID (nodeIdOf) is not Config::nodeId or the name is over kIdentNameMax bytes.
  bool setSigningKey(const uint8_t ed25519Seed[32], const std::string& name = "");
  // X25519-only identity, for nodes without an Ed25519 key: can learn peers
  // from IDENT but cannot send one. Replaces (and stops IDENT for) any signing
  // key set before; DM keys of peers already held are re-derived.
  void setIdentityKey(const uint8_t x25519Secret[32]);
  void setChannelKey(const uint8_t key[32]);
  // Pins a key the host trusts (key file, QR code). Overrides any key learned
  // from IDENT. Call after setIdentityKey / setSigningKey. False on a low-order
  // key or a table full of pinned keys.
  bool addPeerKey(uint32_t id, const uint8_t x25519Public[32]);
  bool hasPeerKey(uint32_t id) const { return peers_.count(id) != 0; }
  // Asks a neighbor for its IDENT (1 hop; the request carries our own IDENT).
  // False without a signing key.
  bool requestIdent(uint32_t id, uint64_t nowMs);

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
  bool storePeer(uint32_t id, const uint8_t xPub[32], bool pinned, uint64_t now);
  void sendIdent(uint32_t dst, uint64_t now);
  void onIdent(const Packet& p, uint64_t now);
  void wantIdent(uint32_t id, uint64_t now);
  void askIdent(uint32_t id, uint64_t now);
  void identTick(uint64_t now);

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

  bool haveIdKey_ = false, haveChanKey_ = false, haveSigKey_ = false;
  std::array<uint8_t, 32> idKey_{}, chanKey_{};
  struct Peer {
    std::array<uint8_t, 32> xPub;   // X25519, compared on every IDENT (TOFU)
    std::array<uint8_t, 32> dm;     // derived DM key
    std::string name;
    bool pinned;                    // from addPeerKey: never evicted
    uint64_t learnedMs;
  };
  std::map<uint32_t, Peer> peers_;
  std::vector<uint8_t> identBody_;  // signed once in setSigningKey
  uint64_t nextIdentAt_ = 0;        // periodic broadcast; 0 = not scheduled yet
  uint64_t identReplyAt_ = 0;       // broadcast answering a request; 0 = none
  uint64_t lastIdentT_ = 0;
  bool identSent_ = false;
  std::map<uint32_t, uint64_t> identWant_;    // neighbor -> when to ask for its IDENT
  std::map<uint32_t, uint64_t> identAsked_;   // neighbor -> last time we asked

  std::map<uint32_t, SosEntry> sos_;   // by originator; newest seq only
  bool sosOn_ = false, sosEver_ = false;
  uint16_t sosSeq_ = 0;
  uint64_t sosLastTx_ = 0;
  std::string sosText_;
  uint8_t sosBattery_ = 0xFF, sosAccuracy_ = 0xFF;
};

}  // namespace gw
