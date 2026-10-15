#include "ridgeline/crypto.h"

#include <cstring>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace rl {
namespace {

const char kIdentCtx[] = "ridgeline-ident-v1";
constexpr size_t kIdentCtxLen = sizeof kIdentCtx - 1;

// "ridgeline-ident-v1" || pub || name: what an IDENT signature covers
size_t identMsg(const uint8_t* pub, const uint8_t* name, size_t nameLen, uint8_t* out) {
  std::memcpy(out, kIdentCtx, kIdentCtxLen);
  std::memcpy(out + kIdentCtxLen, pub, kKeySize);
  if (nameLen) std::memcpy(out + kIdentCtxLen + kKeySize, name, nameLen);
  return kIdentCtxLen + kKeySize + nameLen;
}
constexpr size_t kIdentMsgMax = kIdentCtxLen + kKeySize + kIdentNameMax;

}  // namespace

void x25519Public(const uint8_t secret[kKeySize], uint8_t pub[kKeySize]) {
  crypto_x25519_public_key(pub, secret);
}

void ed25519Public(const uint8_t seed[kKeySize], uint8_t pub[kKeySize]) {
  uint8_t s[kKeySize], sk[64];
  std::memcpy(s, seed, kKeySize);   // crypto_ed25519_key_pair wipes its seed
  crypto_ed25519_key_pair(sk, pub, s);
  crypto_wipe(sk, sizeof sk);
}

void ed25519ToX25519(const uint8_t edPub[kKeySize], uint8_t xPub[kKeySize]) {
  crypto_eddsa_to_x25519(xPub, edPub);
}

void ed25519SecretToX25519(const uint8_t seed[kKeySize], uint8_t xSecret[kKeySize]) {
  uint8_t h[64];
  crypto_sha512(h, seed, kKeySize);
  std::memcpy(xSecret, h, kKeySize);   // X25519 clamps it the same way Ed25519 does
  crypto_wipe(h, sizeof h);
}

uint32_t nodeIdOf(const uint8_t edPub[kKeySize]) {
  uint8_t h[64];
  crypto_sha512(h, edPub, kKeySize);
  return h[0] | (h[1] << 8) | (h[2] << 16) | (static_cast<uint32_t>(h[3]) << 24);
}

bool nodeIdUsable(uint32_t id) { return id != 0 && id != 0xFFFFFFFFu && (id & 0xFFFF) != 0; }

bool identBody(const uint8_t seed[kKeySize], const std::string& name, std::vector<uint8_t>& out) {
  if (name.size() > kIdentNameMax) return false;
  uint8_t s[kKeySize], sk[64], pub[kKeySize], msg[kIdentMsgMax], sig[kSigSize];
  std::memcpy(s, seed, kKeySize);
  crypto_ed25519_key_pair(sk, pub, s);
  size_t n = identMsg(pub, reinterpret_cast<const uint8_t*>(name.data()), name.size(), msg);
  crypto_ed25519_sign(sig, sk, msg, n);
  crypto_wipe(sk, sizeof sk);
  out.assign(pub, pub + kKeySize);
  out.insert(out.end(), sig, sig + kSigSize);
  out.insert(out.end(), name.begin(), name.end());
  return true;
}

bool identCheck(const std::vector<uint8_t>& body, uint32_t src, uint8_t edPub[kKeySize], std::string& name) {
  if (body.size() < kIdentFixed || body.size() > kIdentFixed + kIdentNameMax) return false;
  const uint8_t* pub = body.data();
  if (nodeIdOf(pub) != src || !nodeIdUsable(src)) return false;
  uint8_t msg[kIdentMsgMax];
  size_t nameLen = body.size() - kIdentFixed;
  size_t n = identMsg(pub, body.data() + kIdentFixed, nameLen, msg);
  if (crypto_ed25519_check(body.data() + kKeySize, pub, msg, n) != 0) return false;
  std::memcpy(edPub, pub, kKeySize);
  name.assign(body.begin() + kIdentFixed, body.end());
  return true;
}

bool dmKey(const uint8_t mySecret[kKeySize], const uint8_t theirPub[kKeySize], uint8_t out[kKeySize]) {
  static const char kInfo[] = "ridgeline-dm-v1";
  constexpr size_t kInfoLen = sizeof kInfo - 1;
  uint8_t mine[kKeySize];
  x25519Public(mySecret, mine);
  uint8_t buf[3 * kKeySize + kInfoLen];
  crypto_x25519(buf, mySecret, theirPub);
  static const uint8_t kZero[kKeySize] = {0};
  bool ok = crypto_verify32(buf, kZero) != 0;
  bool mineFirst = std::memcmp(mine, theirPub, kKeySize) < 0;
  std::memcpy(buf + kKeySize, mineFirst ? mine : theirPub, kKeySize);
  std::memcpy(buf + 2 * kKeySize, mineFirst ? theirPub : mine, kKeySize);
  std::memcpy(buf + 3 * kKeySize, kInfo, kInfoLen);
  crypto_blake2b(out, kKeySize, buf, sizeof buf);
  crypto_wipe(buf, sizeof buf);
  if (!ok) crypto_wipe(out, kKeySize);
  return ok;
}

void wipe(void* p, size_t n) { crypto_wipe(p, n); }

}  // namespace rl
