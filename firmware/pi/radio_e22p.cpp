// E22P-915M30S (SX1262) on the Backcountry Signal HAT, via RadioLib + lgpio.
//
// Default pins (BCM) match the HAT and Meshtastic's lora-pimesh-1w-v2 preset:
//   CS 8 (CE0)  DIO1 6  BUSY 5  RESET 18  module EN 26 (held high in config.txt)
#include <RadioLib.h>
#include <sched.h>   // PiHal.h uses sched_yield without including it

#include <cstdio>

#include "hal/RPi/PiHal.h"
#include "host_radio.h"

namespace {

class PiRadio : public HostRadio {
 public:
  PiRadio(PiHal* hal, SX1262* radio, int irqPin) : hal_(hal), radio_(radio), irqPin_(irqPin) {}

  bool channelBusy() override {
    if (hal_->digitalRead(irqPin_)) return true;      // a packet is waiting
    int16_t r = radio_->scanChannel();                // CAD
    radio_->startReceive();
    return r == RADIOLIB_LORA_DETECTED;
  }
  bool transmit(const uint8_t* buf, size_t len) override {
    int16_t st = radio_->transmit(const_cast<uint8_t*>(buf), len);
    radio_->startReceive();
    if (st != RADIOLIB_ERR_NONE) std::fprintf(stderr, "tx failed: %d\n", st);
    return st == RADIOLIB_ERR_NONE;
  }
  size_t receive(uint8_t* buf, size_t cap, float& rssiDbm) override {
    if (!hal_->digitalRead(irqPin_)) return 0;
    size_t len = radio_->getPacketLength();
    bool ok = len > 0 && len <= cap && radio_->readData(buf, len) == RADIOLIB_ERR_NONE;
    if (ok) rssiDbm = radio_->getRSSI();
    radio_->startReceive();
    return ok ? len : 0;
  }

 private:
  PiHal* hal_;
  SX1262* radio_;
  int irqPin_;
};

}  // namespace

std::unique_ptr<HostRadio> makeE22pRadio(const E22pOptions& o, const gw::Phy& phy) {
  // Lives for the whole process, as before.
  auto* hal = new PiHal(0);   // SPI0, CE0 = BCM8
  auto* radio = new SX1262(new Module(hal, o.pinCs, o.pinIrq, o.pinRst, o.pinBusy));
  // "Trail" preset: SF9 / BW125 / CR4/5 / 16-symbol preamble. E22 uses a 1.8 V TCXO.
  int16_t st = radio->begin(o.freqMHz, 125.0, phy.sf, 5, o.syncWord, o.powerDbm,
                            phy.preamble, 1.8, false);
  if (st != RADIOLIB_ERR_NONE) {
    std::fprintf(stderr, "radio init failed: %d (check SPI enabled, EN pin high, wiring)\n", st);
    return nullptr;
  }
  radio->setDio2AsRfSwitch(true);
  radio->setCRC(2);
  radio->startReceive();
  return std::unique_ptr<HostRadio>(new PiRadio(hal, radio, o.pinIrq));
}
