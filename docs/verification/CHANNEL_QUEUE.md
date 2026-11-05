# Channel contention and queue scenarios

- When channelBusy() is true, never call transmit().
- Bound retry backoff and queue capacity under persistent contention.
- SOS priority must not starve all other traffic indefinitely.
- Ensure duplicate messages do not accumulate unlimited queue entries.
- Test clock wraparound and monotonic scheduling.
- Confirm region-specific frequency and airtime restrictions before TX.
