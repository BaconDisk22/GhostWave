#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <bootloader_random.h>
#include <esp_mac.h>
#include <esp_random.h>

#include <cstring>

#include "board.h"
#include "ghostwave/packet.h"

namespace gw {
namespace mcu {

namespace {

constexpr const char* kNs = "gw";

// Blob layout for "peers": count records of 4-byte LE id + 32-byte key.
constexpr size_t kPeerRecord = 4 + kKeySize;

class Nvs {
 public:
  explicit Nvs(bool readOnly) { ok_ = prefs_.begin(kNs, readOnly); }
  ~Nvs() {
    if (ok_) prefs_.end();
  }
  Nvs(const Nvs&) = delete;
  Nvs& operator=(const Nvs&) = delete;
  bool ok() const { return ok_; }
  Preferences* operator->() { return &prefs_; }

 private:
  Preferences prefs_;
  bool ok_;
};

bool getKey(Nvs& nvs, const char* name, Key32& out) {
  return nvs->getBytesLength(name) == out.size() && nvs->getBytes(name, out.data(), out.size()) == out.size();
}

bool savePeers(const Settings& s) {
  uint8_t blob[kMaxStoredPeers * kPeerRecord];
  for (size_t i = 0; i < s.peerCount; ++i) {
    uint8_t* r = blob + i * kPeerRecord;
    uint32_t id = s.peers[i].id;
    for (int b = 0; b < 4; ++b) r[b] = static_cast<uint8_t>(id >> (8 * b));
    std::memcpy(r + 4, s.peers[i].pub.data(), kKeySize);
  }
  Nvs nvs(false);
  return nvs.ok() && nvs->putBytes("peers", blob, s.peerCount * kPeerRecord) == s.peerCount * kPeerRecord;
}

}  // namespace

void secureRandom(uint8_t* out, size_t n) {
  // Without Wi-Fi/BT running the RNG needs this source enabled to be a TRNG.
  // It borrows the SAR ADC, so it must run before anything else uses the ADC.
  bootloader_random_enable();
  esp_fill_random(out, n);
  bootloader_random_disable();
}

bool loadSettings(Settings& s) {
  Nvs nvs(false);
  if (!nvs.ok()) return false;

  s.nodeId = nvs->getUInt("id", 0);
  if (s.nodeId == 0 || s.nodeId == kBroadcast) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    s.nodeId = crc32c(mac, sizeof mac);
    if (s.nodeId == 0 || s.nodeId == kBroadcast) s.nodeId = 0x6E57A11u;
    if (!nvs->putUInt("id", s.nodeId)) return false;
  }

  if (!getKey(nvs, "idkey", s.secret)) {
    secureRandom(s.secret.data(), s.secret.size());
    if (nvs->putBytes("idkey", s.secret.data(), s.secret.size()) != s.secret.size()) return false;
    Serial.println("generated new identity key");
  }
  s.havePsk = getKey(nvs, "psk", s.psk);

  s.fixedPos = nvs->getBool("fixpos", false);
  s.lat = nvs->getDouble("lat", 0);
  s.lon = nvs->getDouble("lon", 0);
  s.outputDbm = nvs->getChar("power", kDefaultOutputDbm);
  s.freqMHz = nvs->getFloat("freq", 914.875f);   // clear of Meshtastic/MeshCore US defaults
  s.responder = nvs->getBool("responder", false);
  s.requireEnc = nvs->getBool("reqenc", false);

  s.peerCount = 0;
  size_t len = nvs->getBytesLength("peers");
  if (len && len % kPeerRecord == 0 && len <= kMaxStoredPeers * kPeerRecord) {
    uint8_t blob[kMaxStoredPeers * kPeerRecord];
    nvs->getBytes("peers", blob, len);
    for (size_t off = 0; off < len; off += kPeerRecord) {
      PeerKey& p = s.peers[s.peerCount++];
      p.id = blob[off] | blob[off + 1] << 8 | blob[off + 2] << 16 | static_cast<uint32_t>(blob[off + 3]) << 24;
      std::memcpy(p.pub.data(), blob + off + 4, kKeySize);
    }
  }
  return true;
}

bool saveFixedPos(const Settings& s) {
  Nvs nvs(false);
  return nvs.ok() && nvs->putBool("fixpos", s.fixedPos) && nvs->putDouble("lat", s.lat) &&
         nvs->putDouble("lon", s.lon);
}

bool savePsk(const Settings& s) {
  Nvs nvs(false);
  if (!nvs.ok()) return false;
  if (!s.havePsk) return nvs->remove("psk") || !nvs->isKey("psk");
  return nvs->putBytes("psk", s.psk.data(), s.psk.size()) == s.psk.size();
}

bool saveRadio(const Settings& s) {
  Nvs nvs(false);
  return nvs.ok() && nvs->putChar("power", s.outputDbm) && nvs->putFloat("freq", s.freqMHz);
}

bool saveFlags(const Settings& s) {
  Nvs nvs(false);
  return nvs.ok() && nvs->putBool("responder", s.responder) && nvs->putBool("reqenc", s.requireEnc);
}

bool savePeer(Settings& s, uint32_t id, const Key32& pub) {
  size_t i = 0;
  while (i < s.peerCount && s.peers[i].id != id) ++i;
  if (i == s.peerCount) {
    if (s.peerCount == kMaxStoredPeers) return false;
    ++s.peerCount;
  }
  s.peers[i] = {id, pub};
  return savePeers(s);
}

}  // namespace mcu
}  // namespace gw
