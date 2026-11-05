# Security and SOS verification matrix

This checklist supplements, but does not replace, automated unit tests. No hardware claims are made.

| Scenario | Required result | Evidence |
| --- | --- | --- |
| Corrupted encrypted DATA tag | Reject, increment authentication failures | Host regression |
| Plaintext DATA with require-encryption | Reject without delivery | Host regression |
| Signed IDENT with mismatched pinned key | Warn and retain original key | Host regression |
| Replayed DATA packet | No duplicate app delivery | Host regression |
| Duplicate SOS sequence | No duplicate emergency notification | Host regression |
| SOS expiry and sequence rollover | Accept fresh sequence after TTL | Host regression |
| SOS stopped while queued | Never transmit cancelled originator frame | Host regression |
| Busy-channel delayed SOS | Retry timer begins at actual attempt | Host regression |
| 24-hour SOS custody | Preserve until TTL, expire after | Virtual-mesh / clock test |
| Radio restart with pending SOS | Recover without accidental replay | Hardware test required |

Before any release, link specific passing test cases to each row. **Do not advertise SOS as verified for emergencies without real-device field validation.**
