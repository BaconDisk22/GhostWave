// Key agreement for DATA encryption (docs/DESIGN.md §9.1). The AEAD itself
// lives in packet.h (aeadSeal / aeadOpen) because it is defined over the frame.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gw {

constexpr size_t kKeySize = 32;

// Public key for an X25519 secret key.
void x25519Public(const uint8_t secret[kKeySize], uint8_t pub[kKeySize]);

// Converts a peer's Ed25519 identity key (DESIGN §3) to its X25519 key.
void ed25519ToX25519(const uint8_t edPub[kKeySize], uint8_t xPub[kKeySize]);

// DM key between us and a peer: BLAKE2b-256(X25519(mySecret, theirPub) ||
// min(pubs) || max(pubs) || "ghostwave-dm-v1"). Both sides get the same key.
// Returns false for a low-order peer key (all-zero shared secret).
bool dmKey(const uint8_t mySecret[kKeySize], const uint8_t theirPub[kKeySize], uint8_t out[kKeySize]);

// Overwrites secret material.
void wipe(void* p, size_t n);

}  // namespace gw
