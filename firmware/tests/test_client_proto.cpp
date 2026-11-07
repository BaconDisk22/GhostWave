// Host tests for the client-link codec (firmware/core/src/client_proto.cpp,
// docs/CLIENT_PROTOCOL.md): round trips, golden bytes, framing, truncated and
// oversized frames, unknown types.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ghostwave/client_proto.h"

using namespace gw::client;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (cond) {                                                         \
      ++g_pass;                                                         \
    } else {                                                            \
      ++g_fail;                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                   \
  } while (0)

static Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
static Bytes key(uint8_t seed) {
  Bytes k(32);
  for (int i = 0; i < 32; ++i) k[i] = static_cast<uint8_t>(seed + i * 7);
  return k;
}

static bool sameTo(const ToRadio& a, const ToRadio& b) {
  return a.reqId == b.reqId && a.kind == b.kind && a.protocolVersion == b.protocolVersion &&
         a.clientName == b.clientName && a.dest == b.dest && a.text == b.text &&
         a.clientMsgId == b.clientMsgId && a.nodeId == b.nodeId && a.ed25519Pub == b.ed25519Pub &&
         a.x25519Pub == b.x25519Pub && a.name == b.name && a.pos == b.pos &&
         a.batteryPct == b.batteryPct && a.accuracyM == b.accuracyM;
}

static bool sameFrom(const FromRadio& a, const FromRadio& b) {
  return a.reqId == b.reqId && a.replayed == b.replayed && a.kind == b.kind &&
         a.protocolVersion == b.protocolVersion && a.nodeId == b.nodeId && a.name == b.name &&
         a.ed25519Pub == b.ed25519Pub && a.x25519Pub == b.x25519Pub && a.legacy == b.legacy &&
         a.firmware == b.firmware && a.maxTextBytes == b.maxTextBytes && a.src == b.src &&
         a.dest == b.dest && a.msgId == b.msgId && a.clientMsgId == b.clientMsgId &&
         a.text == b.text && a.pos == b.pos && a.seq == b.seq && a.accuracyM == b.accuracyM &&
         a.batteryPct == b.batteryPct && a.isNew == b.isNew && a.pinned == b.pinned &&
         a.count == b.count && a.status == b.status && a.error == b.error && a.detail == b.detail;
}

static std::vector<ToRadio> sampleTo() {
  std::vector<ToRadio> v;
  ToRadio m;
  m.reqId = 1, m.kind = ToKind::Hello, m.protocolVersion = 1, m.clientName = "GhostWave iOS";
  v.push_back(m);
  m = {}, m.reqId = 2, m.kind = ToKind::SendText, m.dest = 0xA1B2C3D4, m.text = B("hello \xF0\x9F\x8F\x94"),
  m.clientMsgId = 0xFFFFFFFF;
  v.push_back(m);
  m = {}, m.reqId = 300, m.kind = ToKind::AddContact, m.nodeId = 0x0000BEEF, m.ed25519Pub = key(1), m.name = "bob";
  v.push_back(m);
  m = {}, m.kind = ToKind::AddContact, m.nodeId = 0x12345678, m.x25519Pub = key(9);
  v.push_back(m);
  m = {}, m.reqId = 4, m.kind = ToKind::ListContacts;
  v.push_back(m);
  m = {}, m.reqId = 5, m.kind = ToKind::SetKnownPosition, m.nodeId = 0xFFFFFFFE, m.pos = posFromDegrees(-45.5, 170.25);
  v.push_back(m);
  m = {}, m.reqId = 6, m.kind = ToKind::SosStart, m.text = B("twisted ankle"), m.batteryPct.set(0), m.accuracyM.set(12);
  v.push_back(m);
  m = {}, m.reqId = 7, m.kind = ToKind::SosStart;   // no text, battery/accuracy unknown
  v.push_back(m);
  m = {}, m.reqId = 8, m.kind = ToKind::SosStop;
  v.push_back(m);
  m = {}, m.reqId = 9, m.kind = ToKind::SosAck, m.nodeId = 0x01020304;
  v.push_back(m);
  m = {}, m.reqId = 10, m.kind = ToKind::GetStatus;
  v.push_back(m);
  m = {}, m.reqId = 11, m.kind = ToKind::GetPosition;
  v.push_back(m);
  m = {}, m.reqId = 12, m.kind = ToKind::SetOwnPosition, m.pos = posFromDegrees(0, 0);   // a fix at 0,0
  v.push_back(m);
  m = {}, m.reqId = 13, m.kind = ToKind::SetOwnPosition;   // no fix
  v.push_back(m);
  return v;
}

static std::vector<FromRadio> sampleFrom() {
  std::vector<FromRadio> v;
  FromRadio m;
  m.reqId = 1, m.kind = FromKind::NodeInfo, m.protocolVersion = 1, m.nodeId = 0x9ABCDEF0, m.name = "A";
  m.ed25519Pub = key(3), m.x25519Pub = key(4), m.firmware = "ghostwaved 0.1", m.maxTextBytes = 79;
  v.push_back(m);
  m = {}, m.kind = FromKind::Text, m.replayed = true, m.src = 7, m.msgId = 0xDEADBEEF,
  m.pos = posFromDegrees(46.87, -113.99);
  m.text = Bytes{'h', 'i', 0, 0xFF};
  v.push_back(m);
  m = {}, m.kind = FromKind::Delivered, m.dest = 7, m.msgId = 9, m.clientMsgId = 42;
  v.push_back(m);
  m = {}, m.kind = FromKind::Sos, m.src = 5, m.seq = 65535, m.pos = posFromDegrees(46.9, -114), m.accuracyM.set(8),
  m.text = B("help");
  v.push_back(m);
  m = {}, m.kind = FromKind::Sos, m.src = 5, m.seq = 1;   // no fix, nothing known
  v.push_back(m);
  m = {}, m.kind = FromKind::SosAck, m.src = 6, m.seq = 3;
  v.push_back(m);
  m = {}, m.kind = FromKind::Neighbor, m.nodeId = 8, m.pos = posFromDegrees(1, 2), m.isNew = true;
  v.push_back(m);
  m = {}, m.reqId = 77, m.kind = FromKind::Status;
  m.status = {1, 2, 3, 4, 5, 6, 7, 0x1FFFFFFFFull, 9, 10, 11, true, 13, 14, 15};
  v.push_back(m);
  m = {}, m.reqId = 78, m.kind = FromKind::Position;   // no fix
  v.push_back(m);
  m = {}, m.reqId = 79, m.kind = FromKind::Position, m.pos = posFromDegrees(-89.9999999, -179.9999999);
  v.push_back(m);
  m = {}, m.reqId = 80, m.kind = FromKind::Error, m.error = ErrorCode::TooLong, m.detail = "text over 79 bytes";
  v.push_back(m);
  m = {}, m.reqId = 81, m.kind = FromKind::Contact, m.nodeId = 3, m.name = "c", m.x25519Pub = key(5),
  m.ed25519Pub = key(6), m.pinned = true;
  v.push_back(m);
  m = {}, m.reqId = 81, m.kind = FromKind::ContactsEnd, m.count = 1;
  v.push_back(m);
  m = {}, m.reqId = 82, m.kind = FromKind::SendQueued, m.dest = 1, m.msgId = 2, m.clientMsgId = 3;
  v.push_back(m);
  m = {}, m.reqId = 83, m.kind = FromKind::Ok;
  v.push_back(m);
  m = {}, m.kind = FromKind::PeerIdent, m.nodeId = 4, m.name = "dave", m.isNew = true;
  v.push_back(m);
  m = {}, m.kind = FromKind::KeyChange, m.nodeId = 4, m.ed25519Pub = key(8), m.name = "dave";
  v.push_back(m);
  return v;
}

static void testRoundTrips() {
  std::printf("client proto: round trips\n");
  int i = 0;
  for (const ToRadio& m : sampleTo()) {
    Bytes pb, fr;
    ToRadio back;
    bool ok = encodeTo(m, pb) && decodeTo(pb.data(), pb.size(), back) && sameTo(m, back);
    if (!ok) std::printf("  ToRadio sample %d\n", i);
    CHECK(ok);
    CHECK(frameTo(m, fr) && fr.size() <= kMaxFrame);
    ++i;
  }
  i = 0;
  for (const FromRadio& m : sampleFrom()) {
    Bytes pb, fr;
    FromRadio back;
    bool ok = encodeFrom(m, pb) && decodeFrom(pb.data(), pb.size(), back) && sameFrom(m, back);
    if (!ok) std::printf("  FromRadio sample %d\n", i);
    CHECK(ok);
    CHECK(frameFrom(m, fr) && fr.size() <= kMaxFrame);
    ++i;
  }
  // degrees <-> 1e-7 fixed point
  Pos p = posFromDegrees(46.8700001, -113.9912345);
  CHECK(p.latE7 == 468700001 && p.lonE7 == -1139912345);
}

// Byte-exact frames: the Python reference client and the Dart app must produce
// and accept these (docs/CLIENT_PROTOCOL.md, "Test vectors").
static void testGolden() {
  std::printf("client proto: golden frames\n");
  ToRadio h;
  h.reqId = 1, h.kind = ToKind::Hello, h.protocolVersion = 1, h.clientName = "app";
  Bytes fr;
  CHECK(frameTo(h, fr));
  const uint8_t helloFrame[] = {0x94, 0xC3, 0x0C, 0x00, 0x01, 0x08, 0x01, 0x52,
                                0x07, 0x08, 0x01, 0x12, 0x03, 'a',  'p',  'p'};
  CHECK(fr.size() == sizeof helloFrame && std::memcmp(fr.data(), helloFrame, fr.size()) == 0);

  ToRadio s;
  s.reqId = 2, s.kind = ToKind::SendText, s.dest = 0xA1B2C3D4, s.text = B("hi"), s.clientMsgId = 42;
  fr.clear();
  CHECK(frameTo(s, fr));
  const uint8_t sendFrame[] = {0x94, 0xC3, 0x10, 0x00, 0x01, 0x08, 0x02, 0x5A, 0x0B, 0x0D, 0xD4, 0xC3,
                               0xB2, 0xA1, 0x12, 0x02, 'h',  'i',  0x18, 0x2A};
  CHECK(fr.size() == sizeof sendFrame && std::memcmp(fr.data(), sendFrame, fr.size()) == 0);

  FromRadio d;
  d.kind = FromKind::Delivered, d.dest = 0xA1B2C3D4, d.msgId = 0x01020304, d.clientMsgId = 42;
  fr.clear();
  CHECK(frameFrom(d, fr));
  const uint8_t delFrame[] = {0x94, 0xC3, 0x0F, 0x00, 0x01, 0x62, 0x0C, 0x0D, 0xD4, 0xC3,
                              0xB2, 0xA1, 0x15, 0x04, 0x03, 0x02, 0x01, 0x18, 0x2A};
  CHECK(fr.size() == sizeof delFrame && std::memcmp(fr.data(), delFrame, fr.size()) == 0);

  // negative coordinates are zigzag varints (sint32)
  FromRadio p;
  p.kind = FromKind::Position, p.pos.valid = true, p.pos.latE7 = -1, p.pos.lonE7 = 1;
  Bytes pb;
  CHECK(encodeFrom(p, pb));
  const uint8_t posPb[] = {0x8A, 0x01, 0x06, 0x0A, 0x04, 0x08, 0x01, 0x10, 0x02};
  CHECK(pb.size() == sizeof posPb && std::memcmp(pb.data(), posPb, pb.size()) == 0);
}

static void testTruncated() {
  std::printf("client proto: truncated messages\n");
  for (const ToRadio& m : sampleTo()) {
    Bytes pb;
    encodeTo(m, pb);
    for (size_t n = 0; n < pb.size(); ++n) {
      ToRadio back;
      // a cut inside the request sub-message is malformed; a cut right after
      // req_id is a valid message with no request
      bool ok = decodeTo(pb.data(), n, back);
      if (ok && back.kind != ToKind::None) {
        std::printf("  truncated ToRadio decoded as a request at %zu/%zu\n", n, pb.size());
        CHECK(false);
      }
    }
  }
  for (const FromRadio& m : sampleFrom()) {
    Bytes pb;
    encodeFrom(m, pb);
    for (size_t n = 0; n < pb.size(); ++n) {
      FromRadio back;
      bool ok = decodeFrom(pb.data(), n, back);
      if (ok && back.kind != FromKind::None) {
        std::printf("  truncated FromRadio decoded as an event at %zu/%zu\n", n, pb.size());
        CHECK(false);
      }
    }
  }
  CHECK(true);
}

static void testUnknownAndMalformed() {
  std::printf("client proto: unknown types and malformed input\n");
  ToRadio t;
  // a request from a newer protocol: field 99, length-delimited -> no request
  const uint8_t future[] = {0x08, 0x05, 0x9A, 0x06, 0x02, 0x08, 0x01};
  CHECK(decodeTo(future, sizeof future, t) && t.kind == ToKind::None && t.reqId == 5);
  // unknown fields inside a known request are skipped (every wire type)
  const uint8_t extra[] = {0x5A, 0x18,                               // SendText, 24 bytes
                           0x0D, 0x01, 0x00, 0x00, 0x00,             // dest = 1
                           0x40, 0x96, 0x01,                         // field 8 varint
                           0x49, 1, 2, 3, 4, 5, 6, 7, 8,             // field 9 fixed64
                           0x55, 1, 2, 3, 4,                         // field 10 fixed32
                           0x18, 0x07};                              // client_msg_id = 7
  CHECK(decodeTo(extra, sizeof extra, t) && t.kind == ToKind::SendText && t.dest == 1 && t.clientMsgId == 7);
  // group wire types are refused
  const uint8_t group[] = {0x0B, 0x0C};
  CHECK(!decodeTo(group, sizeof group, t));
  // a known field with the wrong wire type
  const uint8_t wrongType[] = {0x5A, 0x02, 0x08, 0x01};   // SendText.dest as varint, not fixed32
  CHECK(!decodeTo(wrongType, sizeof wrongType, t));
  const uint8_t reqLen[] = {0x0A, 0x00};   // req_id length-delimited
  CHECK(!decodeTo(reqLen, sizeof reqLen, t));
  const uint8_t reqVarint[] = {0x50, 0x01};   // Hello as a varint
  CHECK(!decodeTo(reqVarint, sizeof reqVarint, t));
  // field number 0
  const uint8_t zero[] = {0x00, 0x01};
  CHECK(!decodeTo(zero, sizeof zero, t));
  // an 11-byte varint
  const uint8_t longVarint[] = {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
  CHECK(!decodeTo(longVarint, sizeof longVarint, t));
  // a length running past the end
  const uint8_t overrun[] = {0x52, 0x05, 0x08, 0x01};
  CHECK(!decodeTo(overrun, sizeof overrun, t));
  // empty input: valid, no request
  CHECK(decodeTo(nullptr, 0, t) && t.kind == ToKind::None);
  // last oneof field wins (protobuf rule)
  const uint8_t two[] = {0x92, 0x01, 0x00, 0x9A, 0x01, 0x00};   // GetStatus, then GetPosition
  CHECK(decodeTo(two, sizeof two, t) && t.kind == ToKind::GetPosition);

  FromRadio f;
  const uint8_t futureEvent[] = {0x10, 0x01, 0xAA, 0x06, 0x00};   // replayed, field 101
  CHECK(decodeFrom(futureEvent, sizeof futureEvent, f) && f.kind == FromKind::None && f.replayed);
  // an unknown error code still decodes (apps treat it as generic)
  const uint8_t err[] = {0x92, 0x01, 0x02, 0x08, 0x63};
  CHECK(decodeFrom(err, sizeof err, f) && f.kind == FromKind::Error && static_cast<uint32_t>(f.error) == 99);
  // the encoder refuses kinds outside the schema
  ToRadio bad;
  bad.kind = static_cast<ToKind>(99);
  Bytes pb;
  CHECK(!encodeTo(bad, pb));
}

static void testFraming() {
  std::printf("client proto: framing\n");
  // oversized: a text that does not fit kMaxBody is refused, not cut
  ToRadio big;
  big.kind = ToKind::SendText, big.dest = 1, big.text = Bytes(kMaxBody, 'x');
  Bytes out = {0xAA};
  CHECK(!frameTo(big, out) && out.size() == 1);
  big.text.resize(kMaxBody - 12);   // version 1, request tag 1 + len 2, dest 5, text tag 1 + len 2
  out.clear();
  CHECK(frameTo(big, out) && out.size() == kMaxFrame);

  // two frames in one push, after garbage, then byte by byte
  ToRadio a, b;
  a.reqId = 1, a.kind = ToKind::GetStatus;
  b.reqId = 2, b.kind = ToKind::SendText, b.dest = 3, b.text = B("yo"), b.clientMsgId = 4;
  Bytes stream = {0x00, 0x94, 0x00, 0xC3, 0x94};   // garbage, incl. a lone first magic byte
  CHECK(frameTo(a, stream) && frameTo(b, stream));
  FrameReader r;
  r.push(stream.data(), stream.size());
  uint8_t ver = 0;
  Bytes body;
  ToRadio got;
  CHECK(r.next(ver, body) == FrameReader::Result::Frame && ver == kProtocolVersion);
  CHECK(decodeTo(body.data(), body.size(), got) && sameTo(got, a));
  CHECK(r.next(ver, body) == FrameReader::Result::Frame);
  CHECK(decodeTo(body.data(), body.size(), got) && sameTo(got, b));
  CHECK(r.next(ver, body) == FrameReader::Result::None && r.buffered() == 0 && r.skipped() == 5);

  FrameReader r2;
  int frames = 0;
  for (uint8_t c : stream) {
    r2.push(&c, 1);
    while (r2.next(ver, body) == FrameReader::Result::Frame) ++frames;
  }
  CHECK(frames == 2 && r2.buffered() == 0);

  // a truncated frame waits for the rest
  Bytes one;
  frameTo(b, one);
  FrameReader r3;
  r3.push(one.data(), one.size() - 1);
  CHECK(r3.next(ver, body) == FrameReader::Result::None && r3.buffered() == one.size() - 1);
  r3.push(&one.back(), 1);
  CHECK(r3.next(ver, body) == FrameReader::Result::Frame);

  // a length over kMaxBody, or 0: reported, then resync on the next magic
  for (size_t badLen : {kMaxBody + 1, size_t(0xFFFF), size_t(0)}) {
    Bytes s = {kMagic0, kMagic1, static_cast<uint8_t>(badLen & 0xFF), static_cast<uint8_t>(badLen >> 8), 1, 2, 3};
    frameTo(a, s);
    FrameReader r4;
    r4.push(s.data(), s.size());
    CHECK(r4.next(ver, body) == FrameReader::Result::BadLength);
    CHECK(r4.next(ver, body) == FrameReader::Result::Frame && decodeTo(body.data(), body.size(), got) &&
          got.kind == ToKind::GetStatus);
  }

  // the version byte is passed through for the caller to judge
  Bytes v2;
  uint8_t pb[] = {0x08, 0x01};
  CHECK(frame(pb, sizeof pb, v2, 2));
  FrameReader r5;
  r5.push(v2.data(), v2.size());
  CHECK(r5.next(ver, body) == FrameReader::Result::Frame && ver == 2 && body.size() == 2);
  // the buffer never holds more than one partial frame plus a header
  FrameReader r6;
  Bytes junk(10000, 0x55);
  r6.push(junk.data(), junk.size());
  CHECK(r6.next(ver, body) == FrameReader::Result::None && r6.buffered() == 0);
}

int main() {
  testRoundTrips();
  testGolden();
  testTruncated();
  testUnknownAndMalformed();
  testFraming();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
