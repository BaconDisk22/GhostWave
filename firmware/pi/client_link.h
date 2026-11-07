// Client link for ghostwaved (docs/CLIENT_PROTOCOL.md): a TCP listener that
// speaks the framed protobuf protocol in ghostwave/client_proto.h. Single
// threaded and non-blocking: the daemon's main loop calls poll() each turn.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ghostwave/client_proto.h"
#include "ghostwave/router.h"

struct ClientNodeIdentity {
  uint32_t id = 0;
  std::string name;
  std::array<uint8_t, 32> edPub{}, xPub{};
  bool legacy = false;
};

class ClientLink {
 public:
  struct Hooks {
    // Pins a contact key: (id or 0, ed25519 or empty, x25519 or empty) ->
    // node id, or 0 with `err` set.
    std::function<uint32_t(uint32_t, const gw::client::Bytes&, const gw::client::Bytes&, std::string&)> addPeer;
    // Our own position from the app (valid = false clears it).
    std::function<void(const gw::Position&)> setOwnPosition;
    // Ed25519 keys of pinned peers, for ListContacts.
    const std::map<uint32_t, std::array<uint8_t, 32>>* edKeys = nullptr;
  };

  static constexpr size_t kMaxClients = 4;
  static constexpr size_t kQueueMax = 64;            // events held with no client connected
  static constexpr size_t kOutMax = 64 * 1024;       // per-client send backlog before we drop it
  // A DM always fits: 128-byte frame - 19 header - 20 positions - 16 tag.
  static constexpr uint32_t kMaxTextBytes = 73;

  ClientLink(gw::Router& router, const ClientNodeIdentity& self, const gw::Position& pos, Hooks hooks,
             uint64_t startMs);
  ~ClientLink();
  ClientLink(const ClientLink&) = delete;
  ClientLink& operator=(const ClientLink&) = delete;

  // "PORT", "HOST:PORT" or "[V6HOST]:PORT". Host defaults to 127.0.0.1.
  bool listen(const std::string& spec, std::string& err);
  const std::string& address() const { return addr_; }
  void poll(uint64_t nowMs);

  // Router events (forwarded from the daemon's AppPort).
  void onMessage(uint32_t src, uint32_t msgId, const std::vector<uint8_t>& payload);
  void onDelivered(uint32_t dst, uint32_t msgId);
  void onSos(const gw::SosInfo& s);
  void onSosAck(uint32_t responder, uint16_t seq);
  void onPeerIdent(uint32_t id, const std::string& name, bool firstSeen);
  void onKeyChange(uint32_t id, const uint8_t edPub[32], const std::string& name);
  void onNeighbor(uint32_t id, const gw::Position& pos, bool isNew);

 private:
  struct Client {
    int fd = -1;
    bool hello = false;
    std::string peer;
    gw::client::FrameReader in;
    gw::client::Bytes out;
    bool closing = false;   // close once `out` drains
  };

  void accept();
  void readFrom(Client& c, uint64_t now);
  void flush(Client& c);
  void handle(Client& c, const gw::client::ToRadio& m, uint64_t now);
  void send(Client& c, const gw::client::FromRadio& m);
  void reply(Client& c, uint32_t reqId, gw::client::FromKind kind);
  void error(Client& c, uint32_t reqId, gw::client::ErrorCode code, const std::string& detail);
  // Sends to every client past Hello, or queues if there is none (`queue`).
  void event(const gw::client::FromRadio& m, bool queue);
  size_t ready() const;
  void dropClosed();

  gw::Router& r_;
  ClientNodeIdentity self_;
  const gw::Position& pos_;
  Hooks hooks_;
  uint64_t startMs_;
  int lfd_ = -1;
  std::string addr_;
  std::vector<Client> clients_;
  std::deque<gw::client::FromRadio> queue_;
  std::map<uint32_t, uint32_t> clientIds_;   // mesh msg id -> client_msg_id
  std::deque<uint32_t> clientIdOrder_;
};
