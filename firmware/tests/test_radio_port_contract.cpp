#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "ghostwave/router.h"

// Host-only contract test. This is NOT an RAK4631 SX1262 driver.
class DisabledRadio final : public gw::RadioPort {
 public:
  bool channelBusy() override { return false; }
  bool transmit(const uint8_t*, size_t) override { ++attempts; return false; }
  unsigned attempts = 0;
};
int main() {
  DisabledRadio radio;
  const uint8_t frame[] = {0x01, 0x02};
  const bool sent = radio.transmit(frame, sizeof frame);
  if (sent || radio.attempts != 1) {
    std::fprintf(stderr, "FAIL: disabled radio unexpectedly transmitted or missed call\n");
    return 1;
  }
  return 0;
}
