// Radio backends for ghostwaved: the E22P HAT (RadioLib + lgpio) and a
// hardware-free UDP multicast "virtual radio" for meshes on one machine or LAN.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ghostwave/router.h"
#include "ghostwave/util.h"

class HostRadio : public gw::RadioPort {
 public:
  // Fills `buf` with one received frame; returns its length, or 0 if none.
  virtual size_t receive(uint8_t* buf, size_t cap, float& rssiDbm) = 0;
  // Our current fix (the virtual radio stamps it on every frame).
  virtual void setPosition(const gw::Position& p) { (void)p; }
  // Extra line for the `stats` command.
  virtual void printStats() {}
};

struct E22pOptions {
  float freqMHz = 914.875f;     // clear of Meshtastic/MeshCore US defaults
  int8_t powerDbm = 10;         // SX1262 drive; the E22P PA adds ~8–10 dB. Check your rules.
  uint8_t syncWord = 0x24;      // private: keeps other meshes' frames out at the PHY
  int pinCs = 8, pinIrq = 6, pinRst = 18, pinBusy = 5;
};

struct UdpOptions {
  std::string group = "239.77.0.1";
  int port = 47700;
  std::string iface;            // local address to send/join on; empty = default route
  float txDbm = 24.0f;          // EIRP stamped on our frames (22 dBm + 2 dBi, as ghostsim)
  float rxGainDb = 2.0f;
  double fadeSigmaDb = 5.0;
  bool log = false;             // print a line per frame heard
};

// nullptr (after printing why) if the radio cannot be brought up.
std::unique_ptr<HostRadio> makeE22pRadio(const E22pOptions& o, const gw::Phy& phy);
std::unique_ptr<HostRadio> makeUdpRadio(const UdpOptions& o, const gw::Phy& phy, uint32_t nodeId);

uint64_t nowMs();
