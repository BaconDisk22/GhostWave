#include "ridgeline/packet.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "monocypher.h"

namespace rl {
namespace {

void put16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF;
}
uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t get32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr double kScale = 16777215.0;  // 2^24 - 1

// Flag bits (DESIGN §4.1).
constexpr uint8_t kFlagSrcPos = 1 << 3;
constexpr uint8_t kFlagDstPos = 1 << 2;
constexpr uint8_t kFlagEncrypted = 1 << 1;
constexpr uint8_t kFlagFwdPos = 1 << 0;

// MIC covers every immutable byte: mutable fields are zeroed first.
uint32_t mic(const uint8_t* frame, size_t len, size_t fwdOff, size_t dstOff) {
  uint8_t tmp[kMaxFrame];
  std::memcpy(tmp, frame, len);
  tmp[2] = 0;                                   // hops / copies / wide
  std::memset(tmp + 15, 0, 4);                  // prev_hop, next_hop
  if (fwdOff) std::memset(tmp + fwdOff, 0, kPosSize);
  if (dstOff) std::memset(tmp + dstOff, 0, kPosSize + 1);
  return crc32c(tmp, len);
}

uint8_t flagsOf(const Packet& p) {
  uint8_t flags = static_cast<uint8_t>((p.prio & 3) << 6) |
                  static_cast<uint8_t>((static_cast<uint8_t>(p.mode) & 3) << 4);
  if (p.hasSrcPos) flags |= kFlagSrcPos;
  if (p.hasDstPos) flags |= kFlagDstPos;
  if (p.encrypted) flags |= kFlagEncrypted;
  if (p.hasFwdPos) flags |= kFlagFwdPos;
  return flags;
}

constexpr size_t kAdMax = kHeaderSize + kPosSize + 1;

// AEAD associated data: header with mutable fields zeroed, plus src_pos.
// Route mode and the fwd/dst position flags count as mutable: custodians
// switch GEO <-> UNICAST and add dst_pos en route (DESIGN §5).
size_t aeadAd(const Packet& p, uint8_t* ad) {
  std::memset(ad, 0, kHeaderSize);
  ad[0] = static_cast<uint8_t>((kVersion << 5) | (static_cast<uint8_t>(p.type) & 0x1F));
  ad[1] = static_cast<uint8_t>(flagsOf(p) & ~(0x30 | kFlagDstPos | kFlagFwdPos)) | kFlagEncrypted;
  put32(ad + 3, p.src);
  put32(ad + 7, p.dst);
  put32(ad + 11, p.msgId);
  if (!p.hasSrcPos) return kHeaderSize;
  packPos(p.srcPos, ad + kHeaderSize);
  ad[kHeaderSize + kPosSize] = p.srcAge;
  return kAdMax;
}

void aeadNonce(const Packet& p, uint8_t* nonce12) {
  put32(nonce12, p.src);
  put32(nonce12 + 4, p.msgId);
  std::memset(nonce12 + 8, 0, 4);
}

}  // namespace

uint32_t crc32c(const uint8_t* data, size_t len, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1)));
  }
  return ~crc;
}

void packPos(const Position& p, uint8_t* out) {
  auto lat = static_cast<uint32_t>(std::lround((p.lat + 90.0) / 180.0 * kScale));
  auto lon = static_cast<uint32_t>(std::lround((p.lon + 180.0) / 360.0 * kScale));
  for (int i = 0; i < 3; ++i) {
    out[i] = (lat >> (8 * i)) & 0xFF;
    out[3 + i] = (lon >> (8 * i)) & 0xFF;
  }
}

Position unpackPos(const uint8_t* in) {
  uint32_t lat = in[0] | (in[1] << 8) | (in[2] << 16);
  uint32_t lon = in[3] | (in[4] << 8) | (in[5] << 16);
  return {lat / kScale * 180.0 - 90.0, lon / kScale * 360.0 - 180.0, true};
}

uint8_t ageEncode(uint32_t s) {
  uint32_t m = s / 60;
  if (m < 60) return static_cast<uint8_t>(m);
  uint32_t h = s / 3600;
  if (h < 61) return static_cast<uint8_t>(59 + h);     // 60..119 => 1..60 h
  uint32_t d = std::max<uint32_t>(3, s / 86400);      // round 61..71 h up to 3 d
  if (d < 137) return static_cast<uint8_t>(117 + d);   // 120..253 => 3..136 d
  return 254;
}

uint32_t ageDecode(uint8_t c) {
  if (c == 0xFF) return UINT32_MAX;
  if (c < 60) return c * 60u;
  if (c < 120) return (c - 59u) * 3600u;
  if (c < 254) return (c - 117u) * 86400u;
  return 137u * 86400u;
}

size_t encodedSize(const Packet& p) {
  return kHeaderSize + (p.hasFwdPos ? kPosSize : 0) + (p.hasDstPos ? kPosSize + 1 : 0) +
         (p.hasSrcPos ? kPosSize + 1 : 0) + p.payload.size() + (p.encrypted ? kTagSize : kMicSize);
}

size_t encode(const Packet& p, uint8_t* out, size_t cap) {
  size_t n = encodedSize(p);
  if (n > cap || n > kMaxFrame) return 0;
  uint8_t flags = flagsOf(p);
  uint8_t low = p.mode == Mode::Geo ? (p.wide ? 1 : 0) : (p.copiesLog2 & 0x0F);
  out[0] = static_cast<uint8_t>((kVersion << 5) | (static_cast<uint8_t>(p.type) & 0x1F));
  out[1] = flags;
  out[2] = static_cast<uint8_t>((p.hopsLeft & 0x0F) << 4) | low;
  put32(out + 3, p.src);
  put32(out + 7, p.dst);
  put32(out + 11, p.msgId);
  put16(out + 15, p.prevHop);
  put16(out + 17, p.nextHop);
  size_t off = kHeaderSize, fwdOff = 0, dstOff = 0;
  if (p.hasFwdPos) {
    fwdOff = off;
    packPos(p.fwdPos, out + off);
    off += kPosSize;
  }
  if (p.hasDstPos) {
    dstOff = off;
    packPos(p.dstPos, out + off);
    out[off + kPosSize] = p.dstAge;
    off += kPosSize + 1;
  }
  if (p.hasSrcPos) {
    packPos(p.srcPos, out + off);
    out[off + kPosSize] = p.srcAge;
    off += kPosSize + 1;
  }
  if (!p.payload.empty()) std::memcpy(out + off, p.payload.data(), p.payload.size());
  off += p.payload.size();
  if (p.encrypted) {
    std::memcpy(out + off, p.tag.data(), kTagSize);
    return off + kTagSize;
  }
  put32(out + off, mic(out, off, fwdOff, dstOff));
  return off + kMicSize;
}

bool decode(const uint8_t* in, size_t len, Packet& p) {
  if (len < kHeaderSize + kMicSize || len > kMaxFrame) return false;
  if ((in[0] >> 5) != kVersion) return false;
  uint8_t flags = in[1];
  bool enc = flags & kFlagEncrypted;
  if (enc && len < kHeaderSize + kTagSize) return false;
  p = Packet{};
  p.encrypted = enc;
  p.type = static_cast<PType>(in[0] & 0x1F);
  p.prio = flags >> 6;
  p.mode = static_cast<Mode>((flags >> 4) & 3);
  p.hopsLeft = in[2] >> 4;
  if (p.mode == Mode::Geo) {
    p.wide = in[2] & 1;
  } else {
    p.copiesLog2 = in[2] & 0x0F;
  }
  p.src = get32(in + 3);
  p.dst = get32(in + 7);
  p.msgId = get32(in + 11);
  p.prevHop = get16(in + 15);
  p.nextHop = get16(in + 17);
  size_t off = kHeaderSize, fwdOff = 0, dstOff = 0;
  size_t body = len - (enc ? kTagSize : kMicSize);
  auto need = [&](size_t k) { return off + k <= body; };
  if (flags & kFlagFwdPos) {
    if (!need(kPosSize)) return false;
    fwdOff = off;
    p.hasFwdPos = true;
    p.fwdPos = unpackPos(in + off);
    off += kPosSize;
  }
  if (flags & kFlagDstPos) {
    if (!need(kPosSize + 1)) return false;
    dstOff = off;
    p.hasDstPos = true;
    p.dstPos = unpackPos(in + off);
    p.dstAge = in[off + kPosSize];
    off += kPosSize + 1;
  }
  if (flags & kFlagSrcPos) {
    if (!need(kPosSize + 1)) return false;
    p.hasSrcPos = true;
    p.srcPos = unpackPos(in + off);
    p.srcAge = in[off + kPosSize];
    off += kPosSize + 1;
  }
  p.payload.assign(in + off, in + body);
  if (enc) {
    std::memcpy(p.tag.data(), in + body, kTagSize);
    return true;   // authenticated end to end by aeadOpen, not here
  }
  return get32(in + body) == mic(in, body, fwdOff, dstOff);
}

bool aeadSeal(Packet& p, const uint8_t key[32]) {
  if (p.encrypted) return false;
  p.encrypted = true;
  if (encodedSize(p) > kMaxFrame) {
    p.encrypted = false;
    return false;
  }
  uint8_t ad[kAdMax], nonce[12];
  size_t adLen = aeadAd(p, ad);
  aeadNonce(p, nonce);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, nonce);
  crypto_aead_write(&ctx, p.payload.data(), p.tag.data(), ad, adLen, p.payload.data(), p.payload.size());
  crypto_wipe(&ctx, sizeof ctx);
  return true;
}

bool aeadOpen(const Packet& p, const uint8_t key[32], std::vector<uint8_t>& plain) {
  if (!p.encrypted) return false;
  uint8_t ad[kAdMax], nonce[12];
  size_t adLen = aeadAd(p, ad);
  aeadNonce(p, nonce);
  plain.resize(p.payload.size());
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key, nonce);
  int bad = crypto_aead_read(&ctx, plain.data(), p.tag.data(), ad, adLen, p.payload.data(), p.payload.size());
  crypto_wipe(&ctx, sizeof ctx);
  if (bad) {
    crypto_wipe(plain.data(), plain.size());
    plain.clear();
  }
  return !bad;
}

}  // namespace rl
