// GhostWave client protocol: the link an app (phone, desktop, script) uses to
// drive a node (docs/CLIENT_PROTOCOL.md, schema in proto/ghostwave.proto).
//
// Frame:  0x94 0xC3 len_lo len_hi | version | protobuf ToRadio / FromRadio
//         len counts the version byte and the protobuf bytes.
//
// This is a small hand-written protobuf codec for exactly the messages in the
// schema, so the core needs no protoc / nanopb step and the MCU firmware can
// reuse it over BLE. Apps use any protobuf library with the .proto file.
// Portable: no exceptions, no I/O, no OS calls.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gw {
namespace client {

constexpr uint8_t kMagic0 = 0x94;
constexpr uint8_t kMagic1 = 0xC3;
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kFrameHeader = 4;
// Whole frame, header included. 180 fits one BLE write at ATT MTU 185 (the
// smallest a current iPhone negotiates), so a frame is never split there.
constexpr size_t kMaxFrame = 180;
constexpr size_t kMaxBody = kMaxFrame - kFrameHeader;   // version byte + protobuf

enum class ErrorCode : uint32_t {
  None = 0,
  BadFrame = 1,        // bad magic, bad length or undecodable protobuf
  BadVersion = 2,      // frame version byte (or Hello.protocol_version) unsupported
  UnknownRequest = 3,  // a ToRadio with no request this node understands
  NeedHello = 4,       // request before Hello
  BadArgument = 5,
  TooLong = 6,         // text does not fit one radio frame
  RateLimited = 7,     // SOS started again within a minute
  NotFound = 8,        // e.g. SOS ack for an originator we hold no SOS from
  KeyRejected = 9,     // contact key low-order, wrong for the ID, or table full
  SendFailed = 10,
  TooManyClients = 11,
  NoIdentity = 12,     // operation needs an Ed25519 identity (legacy node)
};

// A fix in 1e-7 degrees. `valid` false = no fix (field absent on the wire).
struct Pos {
  bool valid = false;
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  bool operator==(const Pos& o) const {
    return valid == o.valid && (!valid || (latE7 == o.latE7 && lonE7 == o.lonE7));
  }
};
Pos posFromDegrees(double lat, double lon, bool valid = true);
inline double latDeg(const Pos& p) { return p.latE7 * 1e-7; }
inline double lonDeg(const Pos& p) { return p.lonE7 * 1e-7; }

// Optional uint32: protobuf proto3 `optional`.
struct OptU32 {
  bool has = false;
  uint32_t v = 0;
  void set(uint32_t x) { has = true, v = x; }
  bool operator==(const OptU32& o) const { return has == o.has && (!has || v == o.v); }
};

using Bytes = std::vector<uint8_t>;

// ------------------------------------------------------------- app -> node --

enum class ToKind : uint8_t {
  None = 0,   // no request field, or only fields this version does not know
  Hello = 10,
  SendText = 11,
  AddContact = 12,
  ListContacts = 13,
  SetKnownPosition = 14,
  SosStart = 15,
  SosStop = 16,
  SosAck = 17,
  GetStatus = 18,
  GetPosition = 19,
  SetOwnPosition = 20,
};

struct ToRadio {
  uint32_t reqId = 0;   // echoed in the reply; 0 = caller does not care
  ToKind kind = ToKind::None;
  // Hello
  uint32_t protocolVersion = 0;
  std::string clientName;
  // SendText: dest, text, clientMsgId
  uint32_t dest = 0;
  Bytes text;            // also SosStart text
  uint32_t clientMsgId = 0;
  // AddContact: nodeId, ed25519Pub (or x25519Pub for a legacy node), name
  // SetKnownPosition: nodeId, pos.   SosAck: nodeId.   SetOwnPosition: pos.
  uint32_t nodeId = 0;
  Bytes ed25519Pub, x25519Pub;
  std::string name;
  Pos pos;
  // SosStart: text, batteryPct, accuracyM
  OptU32 batteryPct, accuracyM;
};

// ------------------------------------------------------------- node -> app --

enum class FromKind : uint8_t {
  None = 0,
  NodeInfo = 10,
  Text = 11,
  Delivered = 12,
  Sos = 13,
  SosAck = 14,
  Neighbor = 15,
  Status = 16,
  Position = 17,
  Error = 18,
  Contact = 19,
  ContactsEnd = 20,
  SendQueued = 21,
  Ok = 22,
  PeerIdent = 23,
  KeyChange = 24,
};

struct StatusInfo {
  uint32_t tx = 0, rx = 0, rxBad = 0, rxAuthFail = 0, delivered = 0, forwarded = 0, stored = 0;
  uint64_t airMs = 0;
  uint32_t storeSize = 0, neighbors = 0, sosHeld = 0;
  bool sosActive = false;
  uint32_t uptimeS = 0;
  uint32_t clients = 0, queued = 0;
  bool operator==(const StatusInfo& o) const;
};

struct FromRadio {
  uint32_t reqId = 0;   // the request this answers; 0 for unsolicited events
  bool replayed = false;   // held while no client was connected, sent on Hello
  FromKind kind = FromKind::None;
  // NodeInfo: protocolVersion, nodeId, name, ed25519Pub, x25519Pub, legacy,
  //           firmware, maxTextBytes
  uint32_t protocolVersion = 0;
  uint32_t nodeId = 0;      // also Neighbor / Contact / PeerIdent / KeyChange id
  std::string name;         // also Contact / PeerIdent / KeyChange
  Bytes ed25519Pub, x25519Pub;
  bool legacy = false;
  std::string firmware;
  uint32_t maxTextBytes = 0;
  // Text: src, msgId, text, pos (sender position if the frame carried one)
  // Delivered: dest, msgId, clientMsgId.  SendQueued: dest, msgId, clientMsgId
  uint32_t src = 0, dest = 0, msgId = 0, clientMsgId = 0;
  Bytes text;               // also Sos text
  Pos pos;                  // also Sos / Neighbor / Position
  // Sos: src, seq, pos, accuracyM, batteryPct, text.  SosAck: src (responder), seq
  uint32_t seq = 0;
  OptU32 accuracyM, batteryPct;
  // Neighbor: nodeId, pos, isNew.  PeerIdent: nodeId, name, isNew
  bool isNew = false;
  // Contact: nodeId, name, x25519Pub, ed25519Pub (if known), pinned.
  // ContactsEnd: count
  bool pinned = false;
  uint32_t count = 0;
  StatusInfo status;
  // Error
  ErrorCode error = ErrorCode::None;
  std::string detail;
};

// ------------------------------------------------------------------ codec --

// Protobuf bytes only (no frame header). Decode: false on malformed input;
// unknown fields are skipped, an unknown request leaves kind None.
bool encodeTo(const ToRadio& m, Bytes& out);
bool decodeTo(const uint8_t* p, size_t n, ToRadio& m);
bool encodeFrom(const FromRadio& m, Bytes& out);
bool decodeFrom(const uint8_t* p, size_t n, FromRadio& m);

// Whole frames. Appends to `out`; false (out unchanged) if the body would
// exceed kMaxBody.
bool frameTo(const ToRadio& m, Bytes& out);
bool frameFrom(const FromRadio& m, Bytes& out);
// Wraps an already-encoded protobuf body.
bool frame(const uint8_t* pb, size_t n, Bytes& out, uint8_t version = kProtocolVersion);

// Reassembles frames from a byte stream (TCP, serial, BLE writes). Garbage
// before a magic is skipped; a header with a bad length is reported once and
// skipped byte by byte until the next magic.
class FrameReader {
 public:
  enum class Result { None, Frame, BadLength };
  void push(const uint8_t* p, size_t n);
  // Frame: `version` and `body` (protobuf bytes, version stripped) are set.
  Result next(uint8_t& version, Bytes& body);
  size_t buffered() const { return buf_.size(); }
  size_t skipped() const { return skipped_; }

 private:
  Bytes buf_;
  size_t skipped_ = 0;
};

}  // namespace client
}  // namespace gw
