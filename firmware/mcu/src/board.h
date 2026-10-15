// Per-board pin maps. Every value is taken from Meshtastic's variant files at
// meshtastic/firmware commit 7579bfe (2026-10-15); the file is cited per board.
// Nothing here is guessed: values the variant does not state are marked.
#pragma once

#include <stdint.h>

#if defined(RL_BOARD_TRACKER_V2)
// Source: variants/esp32s3/heltec_wireless_tracker_v2/variant.h
//         (+ pins_arduino.h there for SCK/MISO/MOSI/SS, src/configuration.h for
//          the PA gain curve, src/mesh/LoRaFEMInterface.cpp for FEM sequencing)
#define RL_BOARD_NAME "heltec_wireless_tracker_v2"
constexpr int kPinLoraSck = 9;
constexpr int kPinLoraMiso = 11;
constexpr int kPinLoraMosi = 10;
constexpr int kPinLoraCs = 8;
constexpr int kPinLoraDio1 = 14;    // SX1262 IRQ
constexpr int kPinLoraBusy = 13;
constexpr int kPinLoraRst = 12;
constexpr float kTcxoVolts = 1.8f;  // SX126X_DIO3_TCXO_VOLTAGE
// DIO2 drives the RF switch (SX126X_DIO2_AS_RF_SWITCH); it is wired to the
// KCT8103L FEM's CPS pin.

// KCT8103L front end (PA + LNA). TX: CSD=1 CTX=1, RX with LNA: CSD=1 CTX=0.
#define RL_HAS_KCT8103L 1
constexpr int kPinFemPower = 7;     // LORA_PA_POWER, FEM LDO enable, HIGH = on
constexpr int kPinFemCsd = 4;       // LORA_KCT8103L_PA_CSD, HIGH = chip on
constexpr int kPinFemCtx = 5;       // LORA_KCT8103L_PA_CTX, HIGH = TX / RX bypass, LOW = RX LNA
// TX_GAIN_LORA for HELTEC_WIRELESS_TRACKER_V2: dB of FEM gain at each SX1262
// drive level 0..21 dBm. Meshtastic's own numbers, not measured by us.
constexpr uint8_t kPaGainDb[22] = {14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
                                   14, 13, 13, 13, 12, 12, 11, 10, 9,  8,  7};

#define RL_HAS_GPS 1
constexpr int kPinVext = 3;         // VEXT_ENABLE: powers GPS, GPS LNA and TFT
constexpr bool kVextOn = true;      // VEXT_ON_VALUE HIGH
constexpr int kPinGpsRx = 33;       // GPS_RX_PIN (ESP32 RX <- GPS TX)
constexpr int kPinGpsTx = 34;       // GPS_TX_PIN (ESP32 TX -> GPS RX)
constexpr int kPinGpsReset = 35;    // PIN_GPS_RESET, GPS_RESET_MODE LOW = in reset
constexpr uint32_t kGpsBaud = 115200;   // GPS_BAUDRATE, UC6580

#elif defined(RL_BOARD_HELTEC_V3)
// Source: variants/esp32s3/heltec_v3/variant.h
#define RL_BOARD_NAME "heltec_v3"
constexpr int kPinLoraSck = 9;
constexpr int kPinLoraMiso = 11;
constexpr int kPinLoraMosi = 10;
constexpr int kPinLoraCs = 8;
constexpr int kPinLoraDio1 = 14;
constexpr int kPinLoraBusy = 13;
constexpr int kPinLoraRst = 12;
constexpr float kTcxoVolts = 1.8f;
// DIO2 drives the RF switch. No external PA: output = SX1262 drive.
#define RL_HAS_KCT8103L 0
#define RL_HAS_GPS 0
// VEXT_ENABLE is `Vext` from the board's pins_arduino.h (GPIO 36 in
// platform-espressif32's heltec_wifi_lora_32_V3 variant), active LOW
// (Meshtastic's default VEXT_ON_VALUE). The variant comment says it powers
// "the oled display and the lora antenna boost"; we could not confirm the
// antenna part from a schematic, so it is switched on as Meshtastic does.
constexpr int kPinVext = 36;
constexpr bool kVextOn = false;     // LOW = on

#else
#error "Define RL_BOARD_TRACKER_V2 or RL_BOARD_HELTEC_V3 (see platformio.ini)"
#endif

// Conducted output limit at the antenna port. DESIGN §12 leaves the US rule
// path open (a fixed BW125 channel does not qualify for 15.247's 1 W), so the
// ceiling is held at what a bare SX1262 can do until a compliance decision.
constexpr int kMaxOutputDbm = 22;
constexpr int kDefaultOutputDbm = 20;
