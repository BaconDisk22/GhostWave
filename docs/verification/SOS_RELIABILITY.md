# SOS reliability verification matrix

- No automatic SOS origination on reboot.
- Duplicate SOS sequence does not trigger duplicate user notification.
- Delayed acknowledgment refers to correct source and sequence.
- Missing acknowledgments trigger bounded retries, not endless TX.
- Cancel stops locally scheduled SOS retries.
- Partition and reconnect with four virtual nodes; verify relay and TTL behavior.
- Confirm SOS works with encryption enabled and malformed frames are rejected.
- Hardware validation remains mandatory.
