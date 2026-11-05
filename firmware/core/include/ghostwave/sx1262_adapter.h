// Host-testable SX1262 seam; not a hardware driver and never enables RF.
#pragma once
#include <cstddef>
#include <cstdint>
#include "ghostwave/router.h"
namespace gw {
class Sx1262Driver {
 public:
  virtual ~Sx1262Driver() = default;
  virtual bool ready() const = 0;
  virtual bool busy() = 0;
  virtual bool send(const uint8_t* data, size_t length) = 0;
};
class Sx1262Adapter final : public RadioPort {
 public:
  explicit Sx1262Adapter(Sx1262Driver& driver) : driver_(driver) {}
  bool channelBusy() override { return !driver_.ready() || driver_.busy(); }
  bool transmit(const uint8_t* data, size_t length) override {
    if (!driver_.ready() || !data || length == 0 || length > 255) return false;
    return driver_.send(data, length);
  }
 private:
  Sx1262Driver& driver_;
};
} // namespace gw
