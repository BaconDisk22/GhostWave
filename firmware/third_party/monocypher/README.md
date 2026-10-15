# Monocypher 4.0.2 (vendored)

Pinned copy of the Monocypher core (`src/monocypher.c`, `src/monocypher.h`) and the optional
SHA-512/Ed25519 add-on (`src/optional/monocypher-ed25519.c`, `.h`), unmodified, all from
the same release tarball.

- Source: https://monocypher.org/download/monocypher-4.0.2.tar.gz
- Tarball SHA-512 (matches monocypher.org's published .sha512):
  `bf275d4c53ff94af6cdc723a4e002e9f080f4d1436c86c76bb37870b34807f1d7b32331d8ff8a1aeb369e946f3769021e03e63efac25b82efc5abf54dc084714`
- File SHA-256:
  - afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59  monocypher.c
  - f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374  monocypher.h
  - 7c9b16056cbd27521919e8a6f56a228808b9e718afc42e3d33f28c08e5abdee2  monocypher-ed25519.c
  - bd546edcd468d64e28caa3dbf4b1d6bfad7435c0ce994723fd81aae26405121b  monocypher-ed25519.h
- Licence: dual BSD-2-Clause / CC0-1.0, see LICENCE.md.

Ridgeline uses: `crypto_aead_init_ietf` / `crypto_aead_write` / `crypto_aead_read`
(RFC 8439 ChaCha20-Poly1305), `crypto_x25519`, `crypto_x25519_public_key`,
`crypto_eddsa_to_x25519`, `crypto_blake2b`, `crypto_wipe`, `crypto_verify32`; from the
add-on `crypto_sha512`, `crypto_ed25519_key_pair`, `crypto_ed25519_sign`,
`crypto_ed25519_check` (node IDs and IDENT, DESIGN §3/§4.5).

To upgrade: replace all four files from a new release, update the hashes above, rerun the
tests (they include the RFC 8439 §2.8.2, RFC 8032 §7.1 test 1 and SHA-512 "abc" vectors).
