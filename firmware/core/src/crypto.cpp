#include "ghostwave/crypto.h"

#include <cstring>

#include "monocypher.h"

namespace gw {

void x25519Public(const uint8_t secret[kKeySize], uint8_t pub[kKeySize]) {
  crypto_x25519_public_key(pub, secret);
}

void ed25519ToX25519(const uint8_t edPub[kKeySize], uint8_t xPub[kKeySize]) {
  crypto_eddsa_to_x25519(xPub, edPub);
}

bool dmKey(const uint8_t mySecret[kKeySize], const uint8_t theirPub[kKeySize], uint8_t out[kKeySize]) {
  static const char kInfo[] = "ghostwave-dm-v1";
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

}  // namespace gw
