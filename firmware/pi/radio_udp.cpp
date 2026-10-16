// Virtual radio: every daemon on the multicast group "hears" every frame, and
// VirtualChannel decides from the two positions whether it would have been
// received over the air.
//
// Datagram (little-endian):
//   0  "GWV1"
//   4  u32 sender node id
//   8  u32 sender's datagram counter (a host may deliver one datagram twice)
//  12  i32 lat, i32 lon (1e-7 degrees)
//  20  u8  flags (bit 0: position valid)
//  21  i8  sender EIRP, dBm
//  22  u8  frame length, then the LoRa frame itself
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "host_radio.h"
#include "vchannel.h"

namespace {

constexpr size_t kHeader = 23;
constexpr uint8_t kMagic[4] = {'G', 'W', 'V', '1'};

void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get32(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

class UdpRadio : public HostRadio {
 public:
  UdpRadio(int fd, const sockaddr_in& group, const UdpOptions& o, const gw::Phy& phy, uint32_t id)
      : fd_(fd), group_(group), opt_(o), id_(id), chan_(phy, model(o), id * 0x9E3779B97F4A7C15ull ^ nowMs()) {}
  ~UdpRadio() override { close(fd_); }

  bool channelBusy() override {
    drain();
    return chan_.busy(nowMs());
  }

  bool transmit(const uint8_t* buf, size_t len) override {
    if (len == 0 || len > gw::kMaxFrame) return false;
    uint8_t d[kHeader + gw::kMaxFrame];
    std::memcpy(d, kMagic, 4);
    put32(d + 4, id_);
    put32(d + 8, ++txCount_);
    put32(d + 12, static_cast<uint32_t>(static_cast<int32_t>(std::lround(pos_.lat * 1e7))));
    put32(d + 16, static_cast<uint32_t>(static_cast<int32_t>(std::lround(pos_.lon * 1e7))));
    d[20] = pos_.valid ? 1 : 0;
    d[21] = static_cast<uint8_t>(static_cast<int8_t>(std::lround(opt_.txDbm)));
    d[22] = static_cast<uint8_t>(len);
    std::memcpy(d + kHeader, buf, len);
    chan_.onTransmit(len, nowMs());
    ssize_t n = sendto(fd_, d, kHeader + len, 0, reinterpret_cast<const sockaddr*>(&group_),
                       sizeof group_);
    if (n != static_cast<ssize_t>(kHeader + len)) {
      std::fprintf(stderr, "udp tx failed: %s\n", std::strerror(errno));
      return false;
    }
    return true;
  }

  size_t receive(uint8_t* buf, size_t cap, float& rssiDbm) override {
    drain();
    return chan_.receive(buf, cap, rssiDbm, nowMs());
  }

  void setPosition(const gw::Position& p) override { pos_ = p; }

  void printStats() override {
    const gw::ChannelStats& s = chan_.stats();
    std::printf("vradio heard %u decoded %u out-of-range %u weak %u collided %u nopos %u overflow %u\n",
                s.heard, s.decoded, s.outOfRange, s.weak, s.collided, s.noPos, s.overflow);
  }

 private:
  static gw::ChannelModel model(const UdpOptions& o) {
    gw::ChannelModel m;
    m.rxGainDb = o.rxGainDb;
    m.fadeSigmaDb = o.fadeSigmaDb;
    return m;
  }

  // True if (src, count) was among the last kDedup datagrams; records it if not.
  bool duplicate(uint32_t src, uint32_t count) {
    for (const auto& e : recent_)
      if (e.first == src && e.second == count) return true;
    recent_[recentNext_] = {src, count};
    recentNext_ = (recentNext_ + 1) % recent_.size();
    return false;
  }

  // Moves every waiting datagram onto the virtual air.
  void drain() {
    uint8_t d[kHeader + gw::kMaxFrame + 1];
    for (;;) {
      ssize_t n = recv(fd_, d, sizeof d, MSG_DONTWAIT);
      if (n < 0) return;   // EAGAIN: nothing more
      if (n < static_cast<ssize_t>(kHeader) || std::memcmp(d, kMagic, 4) != 0) continue;
      size_t len = d[22];
      if (static_cast<size_t>(n) != kHeader + len) continue;
      uint32_t src = get32(d + 4);
      if (src == id_) continue;   // our own frame, looped back
      if (duplicate(src, get32(d + 8))) continue;
      gw::Position tx{static_cast<int32_t>(get32(d + 12)) * 1e-7,
                      static_cast<int32_t>(get32(d + 16)) * 1e-7, (d[20] & 1) != 0};
      float txDbm = static_cast<int8_t>(d[21]);
      float p = chan_.onAir(d + kHeader, len, tx, txDbm, pos_, nowMs());
      if (opt_.log) {
        if (!tx.valid || !pos_.valid)
          std::printf("vradio %08x: %zu B, no position, dropped\n", src, len);
        else
          std::printf("vradio %08x: %zu B, %.0f m, %.1f dBm%s\n", src, len, gw::distanceM(tx, pos_),
                      p, p < gw::ChannelModel{}.sensitivityDbm ? ", out of range" : "");
        std::fflush(stdout);
      }
    }
  }

  int fd_;
  sockaddr_in group_;
  UdpOptions opt_;
  uint32_t id_;
  uint32_t txCount_ = 0;
  std::array<std::pair<uint32_t, uint32_t>, 32> recent_{};
  size_t recentNext_ = 0;
  gw::Position pos_;
  gw::VirtualChannel chan_;
};

}  // namespace

std::unique_ptr<HostRadio> makeUdpRadio(const UdpOptions& o, const gw::Phy& phy, uint32_t nodeId) {
  sockaddr_in group{};
  group.sin_family = AF_INET;
  group.sin_port = htons(static_cast<uint16_t>(o.port));
  in_addr ifAddr{};
  ifAddr.s_addr = htonl(INADDR_ANY);
  if (inet_pton(AF_INET, o.group.c_str(), &group.sin_addr) != 1 ||
      !IN_MULTICAST(ntohl(group.sin_addr.s_addr))) {
    std::fprintf(stderr, "bad multicast group %s\n", o.group.c_str());
    return nullptr;
  }
  if (!o.iface.empty() && inet_pton(AF_INET, o.iface.c_str(), &ifAddr) != 1) {
    std::fprintf(stderr, "bad interface address %s\n", o.iface.c_str());
    return nullptr;
  }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    std::fprintf(stderr, "socket: %s\n", std::strerror(errno));
    return nullptr;
  }
  auto fail = [&](const char* what) {
    std::fprintf(stderr, "virtual radio: %s: %s\n", what, std::strerror(errno));
    close(fd);
    return nullptr;
  };
  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) return fail("SO_REUSEADDR");
#ifdef SO_REUSEPORT
  // several daemons on one host all bind the same port
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) return fail("SO_REUSEPORT");
#endif
  sockaddr_in bindAddr{};
  bindAddr.sin_family = AF_INET;
  bindAddr.sin_port = group.sin_port;
  bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&bindAddr), sizeof bindAddr) < 0) return fail("bind");
  ip_mreq mreq{};
  mreq.imr_multiaddr = group.sin_addr;
  mreq.imr_interface = ifAddr;
  if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0)
    return fail("join group (try --vif 127.0.0.1, or check there is a network interface)");
  if (!o.iface.empty() && setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifAddr, sizeof ifAddr) < 0)
    return fail("IP_MULTICAST_IF");
  unsigned char ttl = 1, loop = 1;   // stay on the LAN; hear other daemons on this host
  if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl) < 0) return fail("IP_MULTICAST_TTL");
  if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop) < 0) return fail("IP_MULTICAST_LOOP");
  return std::unique_ptr<HostRadio>(new UdpRadio(fd, group, o, phy, nodeId));
}
