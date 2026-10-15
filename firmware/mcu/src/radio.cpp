#include "radio.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>

#include "board.h"

namespace gw {
namespace mcu {

namespace {

constexpr uint8_t kSyncWord = 0x24;   // private: keeps other meshes' frames out at the PHY
constexpr int kMinDriveDbm = -9;      // SX1262 high-power PA range
constexpr int kMaxDriveDbm = 22;

SX1262 g_sx(new Module(kPinLoraCs, kPinLoraDio1, kPinLoraRst, kPinLoraBusy, SPI));

// SX1262 drive for `out` dBm at the antenna port.
int driveFor(int out) {
#if GW_HAS_KCT8103L
  // Highest drive whose drive + FEM gain does not exceed the target; same
  // search as Meshtastic's LoRaFEMInterface::powerConversion.
  for (int d = static_cast<int>(sizeof kPaGainDb) - 1; d >= 0; --d)
    if (d + kPaGainDb[d] <= out) return d;
  int d = out - kPaGainDb[0];
  return d < kMinDriveDbm ? kMinDriveDbm : d;
#else
  if (out < kMinDriveDbm) return kMinDriveDbm;
  return out > kMaxDriveDbm ? kMaxDriveDbm : out;
#endif
}

}  // namespace

int16_t McuRadio::begin(const Phy& phy, float freqMHz, int outputDbm) {
#if GW_HAS_KCT8103L
  // Power-up order from Meshtastic's LoRaFEMInterface::init: LDO, then chip
  // enable, LNA on for receive.
  pinMode(kPinFemPower, OUTPUT);
  digitalWrite(kPinFemPower, HIGH);
  delay(1);
  pinMode(kPinFemCsd, OUTPUT);
  digitalWrite(kPinFemCsd, HIGH);
  pinMode(kPinFemCtx, OUTPUT);
  femRx();
#endif
  SPI.begin(kPinLoraSck, kPinLoraMiso, kPinLoraMosi, kPinLoraCs);
  outputDbm_ = outputDbm > kMaxOutputDbm ? kMaxOutputDbm : outputDbm;
  driveDbm_ = driveFor(outputDbm_);
  int16_t st = g_sx.begin(freqMHz, phy.bwHz / 1000.0f, phy.sf, 4 + phy.cr, kSyncWord, driveDbm_,
                          phy.preamble, kTcxoVolts, false);
  if (st != RADIOLIB_ERR_NONE) return st;
  st = g_sx.setDio2AsRfSwitch(true);
  if (st == RADIOLIB_ERR_NONE) st = g_sx.setCRC(2);
  if (st == RADIOLIB_ERR_NONE) st = g_sx.startReceive();
  return st;
}

int McuRadio::setOutputDbm(int dbm) {
  outputDbm_ = dbm > kMaxOutputDbm ? kMaxOutputDbm : dbm;
  driveDbm_ = driveFor(outputDbm_);
  g_sx.setOutputPower(driveDbm_);
  g_sx.startReceive();
  return outputDbm_;
}

void McuRadio::femRx() {
#if GW_HAS_KCT8103L
  digitalWrite(kPinFemCtx, LOW);    // receive through the LNA
#endif
}

void McuRadio::femTx() {
#if GW_HAS_KCT8103L
  digitalWrite(kPinFemCtx, HIGH);   // PA path (CPS follows DIO2)
#endif
}

bool McuRadio::channelBusy() {
  if (digitalRead(kPinLoraDio1)) return true;   // a frame is waiting to be read
  int16_t r = g_sx.scanChannel();                // CAD
  g_sx.startReceive();
  return r == RADIOLIB_LORA_DETECTED;
}

bool McuRadio::transmit(const uint8_t* buf, size_t len) {
  femTx();
  int16_t st = g_sx.transmit(buf, len);
  femRx();
  g_sx.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    ++txErrors_;
    Serial.printf("tx failed: %d\n", st);
  }
  return st == RADIOLIB_ERR_NONE;
}

bool McuRadio::poll(uint8_t* buf, size_t cap, size_t& len, float& rssiDbm) {
  if (!digitalRead(kPinLoraDio1)) return false;
  len = g_sx.getPacketLength();
  bool ok = false;
  if (len > 0 && len <= cap) {
    int16_t st = g_sx.readData(buf, len);
    if (st == RADIOLIB_ERR_NONE) {
      rssiDbm = g_sx.getRSSI();
      ok = true;
    } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
      ++crcErrors_;
    }
  }
  g_sx.startReceive();
  return ok;
}

}  // namespace mcu
}  // namespace gw
