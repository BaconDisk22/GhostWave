// Ridgeline wire format (docs/DESIGN.md §4).
#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <vector>

namespace rl {

constexpr uint8_t kVersion = 1;
constexpr uint32_t kBroadcast = 0xFFFFFFFFu;
constexpr size_t kHeaderSize = 19;
constexpr size_t kPosSize = 6;
constexpr size_t kMicSize = 4;      // CRC32-C, unencrypted frames only
constexpr size_t kTagSize = 16;     // Poly1305 tag, encrypted frames (DESIGN §9.1)
constexpr size_t kMaxFrame = 128;

enum class PType : uint8_t {
  Data = 0x01,
  Ack = 0x02,
  LinkAck = 0x03,
  Beacon = 0x04,
  Ident = 0x05,
  Sos = 0x06,
  SosAck = 0x07,
};

enum class Mode : uint8_t { Geo = 0, Spray = 1, Unicast = 2, ScopedFlood = 3 };

enum Priority : uint8_t { kPrioNormal = 0, kPrioHigh = 1, kPrioSos = 2 };

struct Position {
  double lat = 0;
  double lon = 0;
  bool valid = false;
};

struct Packet {
  PType type = PType::Data;
  uint8_t prio = kPrioNormal;
  Mode mode = Mode::Geo;
  bool wide = false;          // GEO: forwarding area widened (retry)
  uint8_t hopsLeft = 15;
  uint8_t copiesLog2 = 0;     // SPRAY: copies = 1 << copiesLog2
  uint32_t src = 0;
  uint32_t dst = kBroadcast;
  uint32_t msgId = 0;
  uint16_t prevHop = 0;       // mutable
  uint16_t nextHop = 0;       // mutable, 0 = anycast
  bool hasFwdPos = false;
  Position fwdPos;            // mutable
  bool hasDstPos = false;
  Position dstPos;            // mutable (refreshed en route)
  uint8_t dstAge = 0xFF;
  bool hasSrcPos = false;
  Position srcPos;
  uint8_t srcAge = 0xFF;
  bool encrypted = false;     // payload is ciphertext and `tag` is its Poly1305 tag
  std::array<uint8_t, kTagSize> tag{};
  std::vector<uint8_t> payload;
};

inline uint16_t shortId(uint32_t id) { return static_cast<uint16_t>(id & 0xFFFF); }

// Returns encoded length, or 0 if it does not fit in `cap`.
size_t encode(const Packet& p, uint8_t* out, size_t cap);
// Returns false on malformed input, wrong version or bad MIC. Encrypted frames
// are parsed but not authenticated (relays have no key): call aeadOpen.
bool decode(const uint8_t* in, size_t len, Packet& out);
size_t encodedSize(const Packet& p);

// ChaCha20-Poly1305 (RFC 8439) over the payload. Nonce = src || msg_id || 0
// (12 bytes LE). Associated data = the 19-byte header with every mutable bit
// zeroed (hops byte, prev/next hop, route mode, has_fwd_pos, has_dst_pos), then
// src_pos + age if present. Set every immutable field before sealing.
// Returns false if `p` is already encrypted or would no longer fit a frame.
bool aeadSeal(Packet& p, const uint8_t key[32]);
// Verifies and decrypts an encrypted packet into `plain`. False on a bad tag.
bool aeadOpen(const Packet& p, const uint8_t key[32], std::vector<uint8_t>& plain);

void packPos(const Position& p, uint8_t* out6);
Position unpackPos(const uint8_t* in6);

// Age of a position observation, 1 byte (DESIGN §4.2). 0xFF = unknown.
uint8_t ageEncode(uint32_t seconds);
uint32_t ageDecode(uint8_t code);   // seconds; UINT32_MAX for unknown

uint32_t crc32c(const uint8_t* data, size_t len, uint32_t crc = 0);

}  // namespace rl
