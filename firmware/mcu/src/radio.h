// SX1262 behind gw::RadioPort, with the board's front end (if any).
#pragma once

#include <cstddef>
#include <cstdint>

#include "ghostwave/router.h"

namespace gw {
namespace mcu {

class McuRadio : public RadioPort {
 public:
  // "Trail" preset (DESIGN §5, matches ghostwaved): SF/CR/preamble from `phy`,
  // BW125, sync word 0x24, CRC on. Returns the RadioLib status (0 = ok).
  int16_t begin(const Phy& phy, float freqMHz, int outputDbm);
  // Output at the antenna port, clamped to kMaxOutputDbm. Returns the value used.
  int setOutputDbm(int dbm);
  int outputDbm() const { return outputDbm_; }
  int driveDbm() const { return driveDbm_; }

  bool channelBusy() override;
  bool transmit(const uint8_t* buf, size_t len) override;

  // Non-blocking: true and fills buf/len/rssi when a good frame was received.
  bool poll(uint8_t* buf, size_t cap, size_t& len, float& rssiDbm);

  uint32_t crcErrors() const { return crcErrors_; }
  uint32_t txErrors() const { return txErrors_; }

 private:
  void femRx();
  void femTx();

  int outputDbm_ = 0, driveDbm_ = 0;
  uint32_t crcErrors_ = 0, txErrors_ = 0;
};

}  // namespace mcu
}  // namespace gw
