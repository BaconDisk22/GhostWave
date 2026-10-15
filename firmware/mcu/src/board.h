// Per-board pin maps. Every value comes from Meshtastic's variant files at
// meshtastic/firmware commit 7579bfe (2026-10-15); the file is cited per
// board. Nothing is guessed: what the variant does not state is marked.
#pragma once

#include <stdint.h>

#if defined(GW_BOARD_TRACKER_V2)
// Source: variants/esp32s3/heltec_wireless_tracker_v2/variant.h
//         (+ pins_arduino.h there for SCK/MISO/MOSI/SS, src/configuration.h for
//          TX_GAIN_LORA, src/mesh/LoRaFEMInterface.cpp for FEM sequencing)
#define GW_BOARD_NAME "heltec_wireless_tracker_v2"
constexpr int kPinLoraSck = 9;
constexpr int kPinLoraMiso = 11;
constexpr int kPinLoraMosi = 10;
constexpr int kPinLoraCs = 8;
constexpr int kPinLoraDio1 = 14;    // SX1262 IRQ
constexpr int kPinLoraBusy = 13;    // variant calls it LORA_DIO2 / SX126X_BUSY
constexpr int kPinLoraRst = 12;
constexpr float kTcxoVolts = 1.8f;  // SX126X_DIO3_TCXO_VOLTAGE
// DIO2 drives the RF switch (SX126X_DIO2_AS_RF_SWITCH); it is wired to the
// KCT8103L FEM's CPS pin.

// KCT8103L front end (PA + LNA). TX: CSD=1 CTX=1. RX with LNA: CSD=1 CTX=0.
#define GW_HAS_KCT8103L 1
constexpr int kPinFemPower = 7;     // LORA_PA_POWER: FEM LDO enable, HIGH = on
constexpr int kPinFemCsd = 4;       // LORA_KCT8103L_PA_CSD: HIGH = chip on
constexpr int kPinFemCtx = 5;       // LORA_KCT8103L_PA_CTX: HIGH = TX / RX bypass, LOW = RX LNA
// TX_GAIN_LORA for HELTEC_WIRELESS_TRACKER_V2: dB of FEM gain at each SX1262
// drive level 0..21 dBm. Meshtastic's numbers, not measured by us.
constexpr uint8_t kPaGainDb[22] = {14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
                                   14, 13, 13, 13, 12, 12, 11, 10, 9,  8,  7};

#define GW_HAS_GPS 1
constexpr int kPinVext = 3;         // VEXT_ENABLE: powers GPS, GPS LNA and TFT
constexpr bool kVextOn = true;      // VEXT_ON_VALUE HIGH
constexpr int kPinGpsRx = 33;       // GPS_RX_PIN (ESP32 RX <- GPS TX)
constexpr int kPinGpsTx = 34;       // GPS_TX_PIN (ESP32 TX -> GPS RX)
constexpr int kPinGpsReset = 35;    // PIN_GPS_RESET; GPS_RESET_MODE LOW = held in reset
constexpr uint32_t kGpsBaud = 115200;   // GPS_BAUDRATE (UC6580)

#elif defined(GW_BOARD_HELTEC_V3)
// Source: variants/esp32s3/heltec_v3/variant.h
#define GW_BOARD_NAME "heltec_v3"
constexpr int kPinLoraSck = 9;
constexpr int kPinLoraMiso = 11;
constexpr int kPinLoraMosi = 10;
constexpr int kPinLoraCs = 8;
constexpr int kPinLoraDio1 = 14;
constexpr int kPinLoraBusy = 13;
constexpr int kPinLoraRst = 12;
constexpr float kTcxoVolts = 1.8f;
// DIO2 drives the RF switch. No external PA: output = SX1262 drive.
#define GW_HAS_KCT8103L 0
#define GW_HAS_GPS 0
// VEXT_ENABLE is `Vext` from the Arduino core's heltec_wifi_lora_32_V3
// pins_arduino.h (GPIO 36), active LOW (Meshtastic's default VEXT_ON_VALUE).
// The variant says it powers "the oled display and the lora antenna boost".
// Not confirmed from a schematic that it touches the RF path, so it is
// switched on exactly as Meshtastic does.
constexpr int kPinVext = 36;
constexpr bool kVextOn = false;     // LOW = on

#else
#error "Define GW_BOARD_TRACKER_V2 or GW_BOARD_HELTEC_V3 (see platformio.ini)"
#endif

// Conducted output ceiling at the antenna port. DESIGN §12 leaves the US rule
// path open (one fixed BW125 channel does not qualify for 15.247's 1 W), so
// this holds every preset at what a bare SX1262 can do until a compliance
// decision. On the Tracker V2 the FEM gain is subtracted before driving the
// SX1262, so the antenna port still sees at most this.
constexpr int kMaxOutputDbm = 22;
constexpr int kDefaultOutputDbm = 20;

// US 902–928 MHz only for now (DESIGN §12); the EU duty-cycle limiter is not
// ported. Keeps a BW125 channel inside the band edges.
constexpr float kMinFreqMHz = 902.0625f;
constexpr float kMaxFreqMHz = 927.9375f;
