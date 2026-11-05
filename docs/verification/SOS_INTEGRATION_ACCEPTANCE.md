# SOS integration acceptance

This checklist does not assert that SOS works on physical radios.

## Host and virtual-radio requirements
- An SOS event must be associated with an origin, sequence and responder.
- Duplicate acknowledgements from the same responder must not count twice within the retention window.
- Distinct responders and distinct SOS events must remain distinguishable.
- Retry budget, jitter, expiry, and queue saturation must have deterministic tests.
- Reboot and power loss must not silently convert a pending SOS into a delivered SOS.
- Private key material and message plaintext must never appear in diagnostic logs.

## Hardware requirements
- Record exact T1000-E and RAK4631 revisions and firmware hashes.
- Test SOS send, receive, acknowledgment, retry, disconnection, reboot and recovery.
- Record serial traces with secrets redacted and note any missed or duplicated delivery.
- Require operator approval and compliant RF configuration before TX tests.

No hardware qualification, transmission authorization, or release approval is granted by this document.
