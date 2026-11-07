// Client link for ghostwaved: TCP transport and request handling
// (docs/CLIENT_PROTOCOL.md).
#include "client_link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "ghostwave/crypto.h"

using namespace gw::client;

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;   // macOS: SO_NOSIGPIPE on each socket instead
#endif

void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

void noSigPipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}

std::string sockName(const sockaddr* sa) {
  char host[INET6_ADDRSTRLEN] = "?";
  int port = 0;
  if (sa->sa_family == AF_INET) {
    auto* in = reinterpret_cast<const sockaddr_in*>(sa);
    inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
    port = ntohs(in->sin_port);
    return std::string(host) + ":" + std::to_string(port);
  }
  if (sa->sa_family == AF_INET6) {
    auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
    inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof host);
    port = ntohs(in6->sin6_port);
  }
  return "[" + std::string(host) + "]:" + std::to_string(port);
}

Pos toPos(const gw::Position& p) { return posFromDegrees(p.lat, p.lon, p.valid); }

gw::Position fromPos(const Pos& p) { return {latDeg(p), lonDeg(p), p.valid}; }

bool usableDest(uint32_t id) { return id != 0 && id != gw::kBroadcast; }

}  // namespace

ClientLink::ClientLink(gw::Router& router, const ClientNodeIdentity& self, const gw::Position& pos,
                       Hooks hooks, uint64_t startMs)
    : r_(router), self_(self), pos_(pos), hooks_(std::move(hooks)), startMs_(startMs) {}

ClientLink::~ClientLink() {
  for (auto& c : clients_) close(c.fd);
  if (lfd_ >= 0) close(lfd_);
}

bool ClientLink::listen(const std::string& spec, std::string& err) {
  std::string host = "127.0.0.1", port = spec;
  size_t colon = spec.rfind(':');
  if (colon != std::string::npos) {
    host = spec.substr(0, colon);
    port = spec.substr(colon + 1);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (host.empty()) host = "127.0.0.1";
  }
  if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos ||
      std::stoul(port) > 65535) {
    err = "bad port in '" + spec + "' (want PORT or HOST:PORT)";
    return false;
  }
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (rc != 0) {
    err = "cannot resolve " + host + ": " + gai_strerror(rc);
    return false;
  }
  for (addrinfo* a = res; a; a = a->ai_next) {
    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, a->ai_addr, a->ai_addrlen) == 0 && ::listen(fd, 4) == 0) {
      setNonBlocking(fd);
      sockaddr_storage ss{};
      socklen_t sl = sizeof ss;
      getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sl);
      addr_ = sockName(reinterpret_cast<sockaddr*>(&ss));
      lfd_ = fd;
      break;
    }
    err = std::string("cannot listen on ") + spec + ": " + std::strerror(errno);
    close(fd);
  }
  freeaddrinfo(res);
  if (lfd_ < 0 && err.empty()) err = "cannot listen on " + spec;
  return lfd_ >= 0;
}

void ClientLink::poll(uint64_t now) {
  if (lfd_ < 0) return;
  accept();
  for (size_t i = 0; i < clients_.size(); ++i) {
    if (clients_[i].fd >= 0 && !clients_[i].closing) readFrom(clients_[i], now);
    if (clients_[i].fd >= 0) flush(clients_[i]);
  }
  dropClosed();
}

void ClientLink::accept() {
  for (int n = 0; n < 4; ++n) {
    sockaddr_storage ss{};
    socklen_t sl = sizeof ss;
    int fd = ::accept(lfd_, reinterpret_cast<sockaddr*>(&ss), &sl);
    if (fd < 0) return;
    setNonBlocking(fd);
    noSigPipe(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    Client c;
    c.fd = fd;
    c.peer = sockName(reinterpret_cast<sockaddr*>(&ss));
    size_t open = std::count_if(clients_.begin(), clients_.end(), [](const Client& x) { return x.fd >= 0; });
    clients_.push_back(std::move(c));
    Client& nc = clients_.back();
    if (open >= kMaxClients) {
      error(nc, 0, ErrorCode::TooManyClients, "at most 4 clients");
      nc.closing = true;
      std::printf("client %s refused: %zu already connected\n", nc.peer.c_str(), open);
    } else {
      std::printf("client %s connected\n", nc.peer.c_str());
    }
    std::fflush(stdout);
  }
}

void ClientLink::readFrom(Client& c, uint64_t now) {
  uint8_t buf[512];
  for (int n = 0; n < 8; ++n) {
    ssize_t got = recv(c.fd, buf, sizeof buf, 0);
    if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      std::printf("client %s disconnected\n", c.peer.c_str());
      std::fflush(stdout);
      close(c.fd);
      c.fd = -1;
      return;
    }
    if (got < 0) break;
    c.in.push(buf, static_cast<size_t>(got));
    uint8_t ver;
    Bytes body;
    for (;;) {
      auto res = c.in.next(ver, body);
      if (res == FrameReader::Result::None) break;
      if (res == FrameReader::Result::BadLength) {
        error(c, 0, ErrorCode::BadFrame, "frame length over 176 or zero");
        continue;
      }
      if (ver != kProtocolVersion) {
        error(c, 0, ErrorCode::BadVersion, "this node speaks client protocol version 1");
        c.closing = true;
        return;
      }
      ToRadio m;
      if (!decodeTo(body.data(), body.size(), m)) {
        error(c, 0, ErrorCode::BadFrame, "undecodable ToRadio");
        continue;
      }
      handle(c, m, now);
      if (c.closing || c.fd < 0) return;
    }
    if (static_cast<size_t>(got) < sizeof buf) break;
  }
}

void ClientLink::flush(Client& c) {
  while (!c.out.empty()) {
    ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), kSendFlags);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
      close(c.fd);
      c.fd = -1;
      return;
    }
    c.out.erase(c.out.begin(), c.out.begin() + n);
  }
  if (c.out.size() > kOutMax) {
    std::printf("client %s dropped: not reading (%zu bytes backlog)\n", c.peer.c_str(), c.out.size());
    std::fflush(stdout);
    close(c.fd);
    c.fd = -1;
    return;
  }
  if (c.closing && c.out.empty()) {
    close(c.fd);
    c.fd = -1;
  }
}

void ClientLink::dropClosed() {
  clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [](const Client& c) { return c.fd < 0; }),
                 clients_.end());
}

size_t ClientLink::ready() const {
  return std::count_if(clients_.begin(), clients_.end(),
                       [](const Client& c) { return c.fd >= 0 && c.hello && !c.closing; });
}

void ClientLink::send(Client& c, const FromRadio& m) {
  if (c.fd < 0) return;
  if (!frameFrom(m, c.out)) {
    // only a too-long event (cannot happen for v1 messages); say so instead
    FromRadio e;
    e.reqId = m.reqId;
    e.kind = FromKind::Error;
    e.error = ErrorCode::TooLong;
    e.detail = "reply too long for a frame";
    frameFrom(e, c.out);
  }
}

void ClientLink::reply(Client& c, uint32_t reqId, FromKind kind) {
  FromRadio m;
  m.reqId = reqId;
  m.kind = kind;
  send(c, m);
}

void ClientLink::error(Client& c, uint32_t reqId, ErrorCode code, const std::string& detail) {
  FromRadio m;
  m.reqId = reqId;
  m.kind = FromKind::Error;
  m.error = code;
  m.detail = detail;
  send(c, m);
}

void ClientLink::handle(Client& c, const ToRadio& m, uint64_t now) {
  const uint32_t req = m.reqId;
  if (m.kind == ToKind::None) {
    error(c, req, ErrorCode::UnknownRequest, "unknown request (newer client?)");
    return;
  }
  if (!c.hello && m.kind != ToKind::Hello) {
    error(c, req, ErrorCode::NeedHello, "send Hello first");
    return;
  }
  switch (m.kind) {
    case ToKind::Hello: {
      if (m.protocolVersion != kProtocolVersion) {
        error(c, req, ErrorCode::BadVersion, "this node speaks client protocol version 1");
        c.closing = true;
        return;
      }
      FromRadio info;
      info.reqId = req;
      info.kind = FromKind::NodeInfo;
      info.protocolVersion = kProtocolVersion;
      info.nodeId = self_.id;
      info.name = self_.name;
      if (!self_.legacy) info.ed25519Pub.assign(self_.edPub.begin(), self_.edPub.end());
      info.x25519Pub.assign(self_.xPub.begin(), self_.xPub.end());
      info.legacy = self_.legacy;
      info.firmware = "ghostwaved";
      info.maxTextBytes = kMaxTextBytes;
      send(c, info);
      bool first = !c.hello;
      c.hello = true;
      std::string who;
      for (unsigned char ch : m.clientName.substr(0, 32)) who += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
      std::printf("client %s hello \"%s\"%s\n", c.peer.c_str(), who.c_str(),
                  first && !queue_.empty() ? (", replaying " + std::to_string(queue_.size()) + " held events").c_str() : "");
      std::fflush(stdout);
      if (first) {
        for (auto& ev : queue_) send(c, ev);
        queue_.clear();
      }
      return;
    }
    case ToKind::SendText: {
      if (!usableDest(m.dest) || m.dest == self_.id) {
        error(c, req, ErrorCode::BadArgument, "bad dest");
        return;
      }
      if (m.text.empty()) {
        error(c, req, ErrorCode::BadArgument, "empty text");
        return;
      }
      if (m.text.size() > kMaxTextBytes) {
        error(c, req, ErrorCode::TooLong, "text over " + std::to_string(kMaxTextBytes) + " bytes");
        return;
      }
      uint32_t mid = r_.send(m.dest, m.text, now);
      if (!mid) {
        error(c, req, ErrorCode::SendFailed, "router refused the message");
        return;
      }
      if (clientIds_.size() >= 256) {
        clientIds_.erase(clientIdOrder_.front());
        clientIdOrder_.pop_front();
      }
      clientIds_[mid] = m.clientMsgId;
      clientIdOrder_.push_back(mid);
      std::printf("client queued id %08x -> %08x\n", mid, m.dest);
      std::fflush(stdout);
      FromRadio q;
      q.reqId = req;
      q.kind = FromKind::SendQueued;
      q.dest = m.dest;
      q.msgId = mid;
      q.clientMsgId = m.clientMsgId;
      send(c, q);
      return;
    }
    case ToKind::AddContact: {
      if (m.name.size() > 32) {
        error(c, req, ErrorCode::BadArgument, "name over 32 bytes");
        return;
      }
      std::string err;
      uint32_t id = hooks_.addPeer ? hooks_.addPeer(m.nodeId, m.ed25519Pub, m.x25519Pub, err) : 0;
      if (!id) {
        error(c, req, ErrorCode::KeyRejected, err);
        return;
      }
      if (!m.name.empty()) r_.setPeerName(id, m.name);
      std::printf("client added contact %08x\n", id);
      std::fflush(stdout);
      reply(c, req, FromKind::Ok);
      return;
    }
    case ToKind::ListContacts: {
      auto peers = r_.peerList();
      for (auto& p : peers) {
        FromRadio ct;
        ct.reqId = req;
        ct.kind = FromKind::Contact;
        ct.nodeId = p.id;
        ct.name = p.name;
        ct.x25519Pub.assign(p.xPub.begin(), p.xPub.end());
        ct.pinned = p.pinned;
        if (hooks_.edKeys) {
          auto it = hooks_.edKeys->find(p.id);
          if (it != hooks_.edKeys->end()) {
            uint8_t x[32];
            gw::ed25519ToX25519(it->second.data(), x);
            if (std::memcmp(x, p.xPub.data(), 32) == 0) ct.ed25519Pub.assign(it->second.begin(), it->second.end());
          }
        }
        send(c, ct);
      }
      FromRadio end;
      end.reqId = req;
      end.kind = FromKind::ContactsEnd;
      end.count = static_cast<uint32_t>(peers.size());
      send(c, end);
      return;
    }
    case ToKind::SetKnownPosition:
      if (!usableDest(m.nodeId) || !m.pos.valid) {
        error(c, req, ErrorCode::BadArgument, "need node_id and pos");
        return;
      }
      r_.addKnownPosition(m.nodeId, fromPos(m.pos), now);
      reply(c, req, FromKind::Ok);
      return;
    case ToKind::SosStart: {
      std::string text(m.text.begin(), m.text.end());
      uint8_t batt = m.batteryPct.has ? static_cast<uint8_t>(std::min<uint32_t>(m.batteryPct.v, 100)) : 0xFF;
      // 4 m steps; 0xFF = unknown, so cap at 0xFE (1016 m)
      uint8_t acc = m.accuracyM.has ? static_cast<uint8_t>(std::min<uint32_t>((m.accuracyM.v + 3) / 4, 0xFE)) : 0xFF;
      if (!r_.startSos(text, batt, acc, now)) {
        error(c, req, ErrorCode::RateLimited, "SOS rate limited, try again in a minute");
        return;
      }
      std::printf("client SOS on\n");
      std::fflush(stdout);
      reply(c, req, FromKind::Ok);
      return;
    }
    case ToKind::SosStop:
      r_.stopSos();
      std::printf("client SOS off\n");
      std::fflush(stdout);
      reply(c, req, FromKind::Ok);
      return;
    case ToKind::SosAck:
      if (!r_.ackSos(m.nodeId, now)) {
        error(c, req, ErrorCode::NotFound, "no SOS held from that node");
        return;
      }
      reply(c, req, FromKind::Ok);
      return;
    case ToKind::GetStatus: {
      auto& s = r_.stats();
      FromRadio st;
      st.reqId = req;
      st.kind = FromKind::Status;
      st.status.tx = s.tx;
      st.status.rx = s.rx;
      st.status.rxBad = s.rxBad;
      st.status.rxAuthFail = s.rxAuthFail;
      st.status.delivered = s.delivered;
      st.status.forwarded = s.forwarded;
      st.status.stored = s.stored;
      st.status.airMs = s.airMs;
      st.status.storeSize = static_cast<uint32_t>(r_.storeSize());
      st.status.neighbors = static_cast<uint32_t>(r_.neighborCount(now));
      st.status.sosHeld = static_cast<uint32_t>(r_.sosCount());
      st.status.sosActive = r_.sosActive();
      st.status.uptimeS = static_cast<uint32_t>((now - startMs_) / 1000);
      st.status.clients = static_cast<uint32_t>(ready());
      st.status.queued = static_cast<uint32_t>(queue_.size());
      send(c, st);
      return;
    }
    case ToKind::GetPosition: {
      FromRadio p;
      p.reqId = req;
      p.kind = FromKind::Position;
      p.pos = toPos(pos_);
      send(c, p);
      return;
    }
    case ToKind::SetOwnPosition:
      if (hooks_.setOwnPosition) hooks_.setOwnPosition(fromPos(m.pos));
      reply(c, req, FromKind::Ok);
      return;
    default:
      error(c, req, ErrorCode::UnknownRequest, "unknown request");
      return;
  }
}

// ------------------------------------------------------------------ events --

void ClientLink::event(const FromRadio& m, bool queue) {
  if (ready()) {
    for (auto& c : clients_)
      if (c.fd >= 0 && c.hello && !c.closing) send(c, m);
    return;
  }
  if (!queue || lfd_ < 0) return;
  if (queue_.size() >= kQueueMax) {
    // make room: the oldest non-SOS event goes first
    auto victim = std::find_if(queue_.begin(), queue_.end(), [](const FromRadio& e) { return e.kind != FromKind::Sos; });
    queue_.erase(victim != queue_.end() ? victim : queue_.begin());
  }
  FromRadio held = m;
  held.replayed = true;
  queue_.push_back(std::move(held));
}

void ClientLink::onMessage(uint32_t src, uint32_t msgId, const std::vector<uint8_t>& payload) {
  FromRadio m;
  m.kind = FromKind::Text;
  m.src = src;
  m.msgId = msgId;
  m.text = payload;
  if (m.text.size() > 128) m.text.resize(128);   // cannot happen: radio frames are 128 bytes
  event(m, true);
}

void ClientLink::onDelivered(uint32_t dst, uint32_t msgId) {
  FromRadio m;
  m.kind = FromKind::Delivered;
  m.dest = dst;
  m.msgId = msgId;
  auto it = clientIds_.find(msgId);
  if (it != clientIds_.end()) m.clientMsgId = it->second;   // kept: a receipt can arrive twice
  event(m, true);
}

void ClientLink::onSos(const gw::SosInfo& s) {
  FromRadio m;
  m.kind = FromKind::Sos;
  m.src = s.src;
  m.seq = s.seq;
  m.pos = toPos(s.pos);
  if (s.accuracy != 0xFF) m.accuracyM.set(s.accuracy * 4u);
  if (s.battery != 0xFF) m.batteryPct.set(s.battery);
  m.text.assign(s.text.begin(), s.text.end());
  event(m, true);
}

void ClientLink::onSosAck(uint32_t responder, uint16_t seq) {
  FromRadio m;
  m.kind = FromKind::SosAck;
  m.src = responder;
  m.seq = seq;
  event(m, true);
}

void ClientLink::onPeerIdent(uint32_t id, const std::string& name, bool firstSeen) {
  FromRadio m;
  m.kind = FromKind::PeerIdent;
  m.nodeId = id;
  m.name = name;
  m.isNew = firstSeen;
  event(m, true);
}

void ClientLink::onKeyChange(uint32_t id, const uint8_t edPub[32], const std::string& name) {
  FromRadio m;
  m.kind = FromKind::KeyChange;
  m.nodeId = id;
  m.ed25519Pub.assign(edPub, edPub + 32);
  m.name = name;
  event(m, true);
}

void ClientLink::onNeighbor(uint32_t id, const gw::Position& pos, bool isNew) {
  FromRadio m;
  m.kind = FromKind::Neighbor;
  m.nodeId = id;
  m.pos = toPos(pos);
  m.isNew = isNew;
  event(m, false);   // live view only: a stale neighbor list is worse than none
}
