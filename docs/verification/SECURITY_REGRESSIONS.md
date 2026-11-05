# Security regression acceptance

- Reject forged IDENT signatures and mismatched node IDs.
- Reject replayed DATA without duplicate delivery.
- Reject malformed encrypted payloads and truncated frames.
- Preserve pinned keys when an unexpected key change arrives.
- Never log secret seeds or decrypted messages.

These are test requirements, not completed tests.
