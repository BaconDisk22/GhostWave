# Monocypher 4.0.2 (vendored)

Pinned copy of the Monocypher core (`src/monocypher.c`, `src/monocypher.h`), unmodified.
Optional SHA-512/Ed25519 add-on is **not** vendored.

- Source: https://monocypher.org/download/monocypher-4.0.2.tar.gz
- Tarball SHA-512 (matches monocypher.org's published .sha512):
  `bf275d4c53ff94af6cdc723a4e002e9f080f4d1436c86c76bb37870b34807f1d7b32331d8ff8a1aeb369e946f3769021e03e63efac25b82efc5abf54dc084714`
- File SHA-256:
  - afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59  monocypher.c
  - f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374  monocypher.h
- Licence: dual BSD-2-Clause / CC0-1.0, see LICENCE.md.

Ridgeline uses: `crypto_aead_init_ietf` / `crypto_aead_write` / `crypto_aead_read`
(RFC 8439 ChaCha20-Poly1305), `crypto_x25519`, `crypto_x25519_public_key`,
`crypto_eddsa_to_x25519`, `crypto_blake2b`, `crypto_wipe`, `crypto_verify16`.

To upgrade: replace both files from a new release, update the hashes above, rerun the tests
(they include the RFC 8439 §2.8.2 vector).
