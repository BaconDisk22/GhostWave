// Build probe only: not firmware for any device; never flash this binary.
#include <Arduino.h>
#include "ghostwave/router.h"

#ifndef NRF52840_XXAA
#error This probe requires an nRF52840 target
#endif

namespace {
class ProbeRadio : public gw::RadioPort {
 public:
  bool channelBusy() override { return false; }
  bool transmit(const uint8_t*, size_t) override { return false; }
};
class ProbeApp : public gw::AppPort {
 public:
  void onMessage(uint32_t, uint32_t, const std::vector<uint8_t>&) override {}
};
ProbeRadio radio;
ProbeApp app;
gw::Config config;
gw::Router* router = nullptr;
}

void setup() {
  config.nodeId = 1;
  router = new gw::Router(config, radio, app);
}
void loop() { router->tick(millis()); }
