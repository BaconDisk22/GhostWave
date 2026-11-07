# GhostWave client protocol, v1

The link an app (the Flutter phone app, a desktop tool, a script) uses to drive one GhostWave
node. This is DESIGN §10 made concrete.

- **Schema:** [`proto/ghostwave.proto`](../proto/ghostwave.proto).
- **Node codec:** `firmware/core` (`ghostwave/client_proto.h`). It is portable and reused by the MCU
  firmware for BLE.
- **Daemon transport:** `ghostwaved --client-tcp [HOST:]PORT` (`firmware/pi/client_link.cpp`).
- **Reference client:** `firmware/pi/tools/gwclient.py`, Python stdlib only.

## 1. Transports

| Transport | Status | Notes |
|---|---|---|
| TCP | done (`ghostwaved`) | Off by default. `--client-tcp 4403` listens on 127.0.0.1:4403. A host must be named explicitly to listen anywhere else (see §7). Up to 4 clients at once. |
| BLE | planned (Heltec) | Nordic-UART-style service: app writes frames to an RX characteristic, the node notifies on TX. The same frames, unchanged. |
| USB serial | planned | The same frames. Garbage before a magic is skipped, so boot logs on the same port are harmless. |

All transports carry one byte stream in each direction, made of frames.

## 2. Framing

```
+------+------+--------+--------+---------+------------------------------+
| 0x94 | 0xC3 | len_lo | len_hi | version | protobuf ToRadio / FromRadio |
+------+------+--------+--------+---------+------------------------------+
               len = 1 + protobuf bytes, 1..176
```

- **Magic:** `0x94 0xC3` is the magic from DESIGN §10, the same idea as Meshtastic's stream framing.
- **len:** little-endian. It counts the version byte plus the protobuf bytes.
- **version:** `1` for this document. A node that gets a different version answers
  `Error{BAD_VERSION}` and closes the link. The app should then show "update the app or the node".
- **Size limit:** a whole frame is at most **180 bytes** (`kMaxFrame`), so `len` ≤ 176. 180 bytes fits
  one BLE write or notification at ATT MTU 185, the smallest a current iPhone negotiates. On BLE every
  frame is then a single write, with no reassembly. The length prefix still makes the stream
  self-delimiting if a transport splits it.
- **Bad length:** if `len` is 0 or over 176, the receiver reports `Error{BAD_FRAME}`. It then drops the
  magic and searches for the next one.
- **Bad protobuf:** an undecodable body gets `Error{BAD_FRAME}`, and the link stays up.

Every v1 message fits easily. The largest is `NodeInfo` at about 110 bytes. A DM's text is capped at
73 bytes anyway (§4).

## 3. Encoding: protobuf (proto3), hand-coded on the node

DESIGN §10 already chose protobuf, and it suits both ends:

- **Dart:** gets generated classes from `protoc --dart_out` and the official `protobuf` package. No
  hand-written parser on the phone.
- **Node:** v1 has about 30 small messages, each made only of varints, fixed32s and byte strings. A
  generic runtime would be overkill. `client_proto.cpp` is a hand-written codec of about 500 lines,
  with no protoc, nanopb or code generation in the firmware build. It is no larger than a TLV
  codec would be. The wire format is still standard protobuf, so any protobuf tool can decode a
  capture.
- **Why not TLV:** it is simpler on paper, but every client would have to hand-write the codec, and
  it gives no schema evolution rules.
- **Why not nanopb:** it remains an option for the MCU if the schema grows a lot. It produces the
  same bytes.

Schema evolution rules are in the header of `ghostwave.proto`. In short: never renumber or retype a
field. Add new requests and events as new oneof fields. A receiver skips unknown fields. An unknown
request gets `Error{UNKNOWN_REQUEST}`.

Encoding conventions:
- **Node IDs and message IDs** are `fixed32`: they are random 32-bit values.
- **Positions** are `sint32` in units of 1e-7°. An absent `Position` means "no fix", so a real fix at
  0,0 is still sent as a present but empty sub-message.
- **Text from the air** (`TextMessage.text`, SOS text, names) is `bytes`. It is usually UTF-8, but
  the radio does not guarantee that. Decode it leniently, e.g. Dart `utf8.decode(b, allowMalformed: true)`.
- **`optional uint32`** is used where 0 is meaningful (battery %, accuracy). Absent means "unknown".

## 4. Messages

Every `ToRadio` may carry a `req_id`. Each reply to it echoes that `req_id`. Unsolicited events
have `req_id` 0. A request gets exactly one reply: its result, `Ok` or `Error`. The exception is
`ListContacts`, which gets zero or more `Contact` and then `ContactsEnd`.

### App → node (`ToRadio`)

| # | Request | Fields | Reply |
|---|---|---|---|
| 10 | `Hello` | `protocol_version` (=1), `client_name` | `NodeInfo`, then replayed events (§5) |
| 11 | `SendText` | `dest`, `text` (≤ 73 bytes), `client_msg_id` | `SendQueued{dest, msg_id, client_msg_id}`; later the event `Delivered` |
| 12 | `AddContact` | `node_id` (0 = derive from the key), `ed25519_pub`, or `x25519_pub` for a legacy node; `name` (≤ 32 bytes) | `Ok` / `KEY_REJECTED` |
| 13 | `ListContacts` | none | `Contact`…, `ContactsEnd{count}` |
| 14 | `SetKnownPosition` | `node_id`, `pos` | `Ok` |
| 15 | `SosStart` | `text` (cut to 60 bytes), optional `battery_pct`, optional `accuracy_m` | `Ok` / `RATE_LIMITED` |
| 16 | `SosStop` | none | `Ok` |
| 17 | `SosAck` | `node_id` (the SOS originator) | `Ok` / `NOT_FOUND` |
| 18 | `GetStatus` | none | `Status` |
| 19 | `GetPosition` | none | `OwnPosition{pos}` |
| 20 | `SetOwnPosition` | `pos` (absent = clear) | `Ok`. The phone's GPS becomes the node's position, for nodes without GPS. A node GPS fix overrides it. |

Notes on the requests:

- **Hello comes first.** Any other request before it gets `NEED_HELLO`.
- **Text limit:** 73 bytes is `NodeInfo.max_text_bytes`. It is the most that always fits a 128-byte
  radio frame: header 19 + forward, destination and source positions 20 + Poly1305 tag 16. Longer
  text gets `TOO_LONG`, never a silent cut. The app should count UTF-8 bytes, not characters.
- **AddContact is the QR scan.** A contact card is `node_id` + `ed25519_pub` + a display name.
  The key must hash to the ID (DESIGN §3), or the request gets `KEY_REJECTED`. A pinned key overrides
  one learned over the air (TOFU).
- **SetKnownPosition** seeds where a node is believed to be. With it, DMs route geographically
  instead of waiting for SPRAY encounters. The app should send it for contacts with a known
  location.
- **SOS:** accuracy is carried in 4 m steps on the air, so it comes back rounded up. A second
  `SosStart` within a minute of the last one gets `RATE_LIMITED` (DESIGN §8). Calling `SosStart`
  again while an SOS is active updates its text.

### Node → app (`FromRadio`)

| # | Event | Fields | When |
|---|---|---|---|
| 10 | `NodeInfo` | `protocol_version`, `node_id`, `name`, `ed25519_pub`, `x25519_pub`, `legacy`, `firmware`, `max_text_bytes` | reply to `Hello` |
| 11 | `TextMessage` | `src`, `msg_id`, `text` | a DM for us arrived |
| 12 | `Delivered` | `dest`, `msg_id`, `client_msg_id` | end-to-end receipt for our DM |
| 13 | `SosAlert` | `src`, `seq`, `pos`, `accuracy_m`, `battery_pct`, `text` | a new SOS, or a newer seq of one already held |
| 14 | `SosAckReceived` | `responder`, `seq` | someone acknowledged our SOS |
| 15 | `Neighbor` | `node_id`, `pos`, `is_new` | a neighbor's beacon (live only, not queued) |
| 16 | `Status` | counters, `store_size`, `neighbors`, `sos_held`, `sos_active`, `uptime_s`, `clients`, `queued` | reply to `GetStatus` |
| 17 | `OwnPosition` | `pos` | reply to `GetPosition` |
| 18 | `Error` | `code`, `detail` | a request failed, or the stream is bad |
| 19 | `Contact` | `node_id`, `name`, `x25519_pub`, `ed25519_pub`, `pinned` | reply to `ListContacts` |
| 20 | `ContactsEnd` | `count` | ends `ListContacts` |
| 21 | `SendQueued` | `dest`, `msg_id`, `client_msg_id` | reply to `SendText` |
| 22 | `Ok` | none | success with no data |
| 23 | `PeerIdent` | `node_id`, `name`, `is_new` | a verified IDENT taught a new key, or a new name |
| 24 | `KeyChange` | `node_id`, `ed25519_pub`, `name` | a signed IDENT carries a different key (DESIGN §9.2). Warn the user. The node keeps the old key. |

Notes on the events:

- **Matching receipts:** match `Delivered` to the UI message by `client_msg_id`, which the app chose.
  `msg_id` also works. A receipt can arrive more than once, so treat it as idempotent.
- **No receipt is not a failure.** A missing `Delivered` does not mean the message failed. DTN
  delivery can take hours, and the receipt may wait on the far side of a partition (DESIGN §5.4).
  Show the message as "sent" (queued) until `Delivered` arrives.
- **Only opened messages reach the app.** A DM sealed with a key the node cannot match (no
  key for the sender yet, and no shared channel key) is dropped on the node. So the app only
  ever sees plaintext the node could decrypt.
- **Contact.ed25519_pub** is only filled for keys pinned from an Ed25519 key since the daemon
  started. Keys learned over IDENT give only `x25519_pub`.

### Error codes

| Code | Name | Meaning |
|---|---|---|
| 1 | `BAD_FRAME` | bad length, or an undecodable protobuf |
| 2 | `BAD_VERSION` | frame version or `Hello.protocol_version` not 1. The link is closed. |
| 3 | `UNKNOWN_REQUEST` | no request this node knows (newer app) |
| 4 | `NEED_HELLO` | a request before `Hello` |
| 5 | `BAD_ARGUMENT` | e.g. dest 0, broadcast or self; empty text; missing position |
| 6 | `TOO_LONG` | text over `max_text_bytes` |
| 7 | `RATE_LIMITED` | SOS restarted within a minute |
| 8 | `NOT_FOUND` | `SosAck` for a node we hold no SOS from |
| 9 | `KEY_REJECTED` | the key does not hash to the ID, is low-order, or the peer table is full of pinned keys |
| 10 | `SEND_FAILED` | the router refused the message |
| 11 | `TOO_MANY_CLIENTS` | a 5th connection. Sent, then the link is closed. |
| 12 | `NO_IDENTITY` | the operation needs an Ed25519 identity (reserved) |

## 5. No client connected: the event queue

While no client has completed `Hello`, the node holds these events in memory: `TextMessage`,
`Delivered`, `SosAlert`, `SosAckReceived`, `PeerIdent` and `KeyChange`. `Neighbor` is not held,
because it is only useful live.

- **Capacity:** at most 64 events. When the queue is full, the oldest non-SOS event is dropped.
  SOS alerts are dropped last.
- **Replay:** the first client to send `Hello` gets `NodeInfo`, then every held event in order, each
  with `replayed = true`. The queue then empties.
- **Several clients:** live events go to every connected client that has sent `Hello`. Only the
  first client to connect after a quiet period gets the replay.
- **Lifetime:** the queue lives in RAM, so a daemon restart loses it. The radio side still holds
  undelivered DMs in its own store-carry-forward queue.

**For the app:** de-duplicate by (`src`, `msg_id`), since a replayed message may already be on
screen after a reconnect. Show replayed SOS alerts prominently. They may be hours old, and their
`seq` and position say how stale they are.

## 6. A session

```
app                                   node
 |-- Hello{v=1, "GhostWave iOS"} ----->|
 |<----------------- NodeInfo ---------|  (req_id echoed)
 |<----- TextMessage{replayed} --------|  held while no app was connected
 |-- AddContact{id, ed25519, "Bob"} -->|
 |<-------------------- Ok ------------|
 |-- SetKnownPosition{bob, pos} ------>|
 |<-------------------- Ok ------------|
 |-- SendText{bob, "hi", cmid 7} ----->|
 |<------- SendQueued{msg_id M} -------|
 |                 ...                 |  radio
 |<-- Delivered{bob, M, cmid 7} -------|
 |<---- SosAlert{src, pos, text} ------|
 |-- SosAck{src} --------------------->|
 |<-------------------- Ok ------------|
```

Test vectors are byte-exact frames checked by `firmware/tests/test_client_proto.cpp`:

| Message | Frame (hex) |
|---|---|
| `ToRadio{req_id 1, Hello{1, "app"}}` | `94c30c0001 0801 5207 0801 1203 617070` |
| `ToRadio{req_id 2, SendText{a1b2c3d4, "hi", 42}}` | `94c3100001 0802 5a0b 0dd4c3b2a1 12026869 182a` |
| `FromRadio{Delivered{a1b2c3d4, 01020304, 42}}` | `94c30f0001 620c 0dd4c3b2a1 1504030201 182a` |

## 7. Security

- **No authentication yet:** v1 has no client authentication. Whoever reaches the TCP port can read
  the node's DMs, send as the node, pin keys and raise or ack SOS. This is why `--client-tcp` binds
  to 127.0.0.1 unless a host is named.
- **Binding elsewhere:** `--client-tcp 0.0.0.0:4403`, or a LAN address, is for testing on a trusted
  network. The daemon prints a warning when it does this.
- **BLE:** the Heltec link will rely on BLE pairing (passkey shown on the node) for the same
  property.
- **Planned:** a `Hello` that proves knowledge of a pairing secret. TLS is not planned.
- **Keys:** private keys never cross the link. `NodeInfo` and `Contact` carry public keys only.

## 8. Notes for the Flutter app (Part 2)

- **Code generation:** `protoc --dart_out=lib/gen -I proto proto/ghostwave.proto`. Copy or submodule
  `proto/` from this repo, and pin it to the protocol version.
- **Framing:** implement the framer as a `StreamTransformer<List<int>, FromRadio>`. Skip to the magic,
  read `len`, check the version, then call `FromRadio.fromBuffer`. The Python `FrameReader` in
  `gwclient.py` is a 25-line model.
- **Request IDs:** use a counter per connection, and keep a map from `req_id` to `Completer`. Route
  `req_id == 0` to an event stream.
- **Reconnect:** on every reconnect, send `Hello` again. Then de-duplicate replayed messages (§5).
- **Text limit:** enforce `max_text_bytes` in the composer by UTF-8 byte count.
- **Contact card / QR:** carry `ghostwave:` + node ID (8 hex) + Ed25519 key (64 hex) + name. The node
  only needs ID + key (`AddContact`). The format of the QR string itself belongs to the app spec.
- **Testing:** `firmware/pi/tools/vmesh_lan.sh --lan` runs 4–6 virtual nodes whose client ports a
  phone on the same Wi-Fi can reach (README, "Client link").
