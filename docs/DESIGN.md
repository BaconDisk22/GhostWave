# GhostWave — Protocol Design v0.1

> A LoRa mesh built for people who move through places with no coverage:
> hikers, SAR teams, hunters, overlanders, backcountry skiers.

Status: **draft / prototype**. Everything here is open to change until v1.0; the packet
`version` field exists so it can.

---

## 1. Goals and non-goals

### Goals (the four pillars)

| # | Pillar | User-visible promise |
|---|---|---|
| P1 | **Delay-tolerant delivery (DTN)** | "My message got through, even though nobody was in range when I sent it." |
| P2 | **Geographic routing** | "It keeps working when everyone is moving." |
| P3 | **Congestion control** (duplicate-free forwarding, 1-hop beacons, airtime budget) | "The mesh stays usable when it gets busy." |
| P4 | **SOS priority lane** | "An emergency always goes first and keeps trying." |

Plus, for adoption: a **bridge** to Meshtastic and MeshCore (out of scope for this
document; see §11).

### Non-goals (v1)

- Internet-scale routing. Target: 10–500 nodes per region.
- Voice/streaming. Text, position, small telemetry only.
- Per-link adaptive data rate (SX126x can only listen on one SF at a time; deferred to v2).
- Forward secrecy / group key rotation (deferred to v2; §9.3).

---

## 2. Roles and assumptions

- **Every node is equal** at the protocol level. There is no "repeater vs companion"
  split. A node with mains power and a good site is just a node that is always on and
  rarely moves — the routing naturally prefers it because it makes progress.
- Most nodes have **GPS**. Nodes without GPS can't act as GEO relays (no position to measure
  progress from), but they still send, receive and carry spray copies. They are routed
  *to* via their last-known position, which may be supplied manually (fixed install) or
  learned from neighbors.
- Default PHY preset **"Trail"**: 915 MHz (US) / 869.525 MHz (EU), SF9, BW125 kHz, CR 4/5,
  16-symbol preamble, explicit header, CRC on. One preset per region; all nodes on a
  channel share it.

---

## 3. Identity and addressing

- Each node owns an **Ed25519** keypair. The **node ID** is the first 4 bytes of
  `SHA-512(pubkey)`, read little-endian like every other ID on the wire. `0xFFFFFFFF` =
  broadcast. (Earlier drafts said SHA-256. SHA-512 is used because Ed25519 already needs it
  and Monocypher has no SHA-256; any 4-byte truncation of a good hash is equally strong.)
- A key whose ID is `0`, `0xFFFFFFFF`, or has low 16 bits `0` (short ID 0 means "anycast"
  in `next_hop`) is unusable: the node generates another key.
- The **short ID** (2 bytes) is the low 16 bits of the node ID and is used only for
  link-local fields (previous hop / next hop), where a collision costs one wasted
  transmission, not a misdelivery.
- Public keys are exchanged out of band (QR code) or via a signed `IDENT` packet (§4.5).

---

## 4. Packet format

All multi-byte integers are **little-endian**. Positions are packed as 24-bit fixed
point (see §4.2). The common header is **19 bytes**.

### 4.1 Common header

```
 off  size  field
 0    1     ver_type     bits 7..5 = version (1), bits 4..0 = packet type
 1    1     flags        bits 7..6 = priority (0 normal, 1 high, 2 SOS, 3 reserved)
                         bits 5..4 = route mode (0 GEO, 1 SPRAY, 2 UNICAST, 3 SCOPED_FLOOD)
                         bit  3    = has_src_pos
                         bit  2    = has_dst_pos
                         bit  1    = encrypted
                         bit  0    = has_fwd_pos
 2    1     hops         bits 7..4 = hops_left (0..15)
                         bits 3..0 = SPRAY/UNICAST: copies (log2); GEO: bit 0 = wide area
 3    4     src          node ID
 7    4     dst          node ID (0xFFFFFFFF broadcast)
 11   4     msg_id       random per message; (src, msg_id) is the dedup key
 15   2     prev_hop     short ID of the transmitter of THIS frame        (mutable)
 17   2     next_hop     short ID of intended receiver, 0 = anycast         (mutable)
```

Then, by flags:

```
 +6   fwd_pos   transmitter's position at TX time (always present on GEO frames)  (mutable)
 +7   dst_pos   target position + 1-byte age (present if has_dst_pos)            (mutable)
 +7   src_pos   source position + 1-byte age (present if has_src_pos)
 ...  payload
 +16  tag       AEAD tag (ChaCha20-Poly1305) if encrypted, else 4-byte CRC32-C MIC
```

**Mutable fields** (`hops`, `prev_hop`, `next_hop`, `fwd_pos`, `dst_pos`) change hop by hop
and are **excluded from the AEAD associated data**. So are the route-mode bits and the
`has_fwd_pos` / `has_dst_pos` flags, because custodians switch a message between GEO and
UNICAST and add `dst_pos` en route. Everything else is authenticated end to end (§9.1).

### 4.2 Position encoding (6 bytes + optional 1-byte age)

- `lat24 = round((lat + 90) / 180 * (2^24 - 1))` → ~1.2 m resolution
- `lon24 = round((lon + 180) / 360 * (2^24 - 1))` → ~2.4 m at the equator
- **age** (1 byte, log-ish): `0..59` = minutes, `60..119` = (n-59) hours, `120..253` =
  (n-117) days (3–136; 61–71 h rounds up to 3 days), `254` = older, `255` = unknown. This is the age of the position
  **observation**, so any node can tell which of two positions for the same node is
  fresher.

### 4.3 Packet types

| Type | Name | Purpose |
|---|---|---|
| 0x01 | `DATA` | Application payload (text, position, telemetry) |
| 0x02 | `ACK` | End-to-end delivery receipt; routed GEO back to `src_pos` |
| 0x03 | `LINK_ACK` | Hop-by-hop ack: UNICAST receipt, "I already have custody" to a retrying upstream, and "it arrived" from a GEO frame's final recipient. Body: acked type (1) + acked src (4); `msg_id` in the header; `next_hop` = who it's for. |
| 0x04 | `BEACON` | 1-hop presence: own position, velocity, location gossip, DTN summary |
| 0x05 | `IDENT` | Signed public key + name (sent on request or at low rate) |
| 0x06 | `SOS` | Emergency; see §8 |
| 0x07 | `SOS_ACK` | "Help acknowledged" from a responder |
| 0x1F | reserved for extension (next byte = extended type) |

### 4.4 Beacon body

```
 0   1    beacon_flags   bit0 has_gps, bit1 stationary, bit2 mains_power, bit3 gateway,
                         bit4 carrying_sos
 1   2    velocity       heading (7 bits, 2.8° steps) + speed (9 bits, 0.1 m/s)
 3   1    util           channel utilization this node measures, 0..255 = 0..100 %
 4   1    store_count    number of DTN messages carried
 5   8    dtn_summary    64-bit Bloom filter over carried (src,msg_id) keys, k = 2
                         (DTN store entries and held SOS frames)
 13  11×n gossip         up to 3 entries: node ID (4) + pos (6) + age (1)
```

The beacon's own position is in `fwd_pos`. Size with 3 gossip entries: 19 + 6 + 13 + 33 + 4 =
**75 bytes**. (The simulator models 64 B; the 11 extra bytes don't change any conclusion.)

### 4.5 IDENT body

```
 0   32   pubkey        sender's Ed25519 public key
 32  64   signature     Ed25519 (RFC 8032) over "ghostwave-ident-v1" || pubkey || name
 96  0..9 name          UTF-8, not NUL-terminated; length = body length - 96
```

Header: route mode UNICAST, `hops_left` 0 (one hop, never forwarded), no positions, CRC MIC
(not encrypted). `dst` = broadcast announces the sender; `dst` = a node ID *requests* that
node's IDENT (and announces the sender to everyone in range, since the body is the same).
With a 9-byte name the frame is 19 + 105 + 4 = **128 bytes**, the maximum frame.

The receiver accepts an IDENT only if the body length is 96..105, `src` = node ID of
`pubkey` (§3), and the signature verifies. There is no timestamp: the key is the identity, so
replaying someone's IDENT only repeats it. Then:

- **Unknown `src`:** trust on first use. Store `X25519(pubkey)` (the birational map, §9.1) as
  that peer's DM key and report the name to the app.
- **Known `src`, same key:** update the name if it changed.
- **Known `src`, different key:** keep the old key and raise a key-change warning to the app,
  which can accept the new key explicitly. An honest node can't trigger this (its ID is a hash
  of its key), so it means a brute-forced 32-bit ID collision (§9.2) or a mis-pinned key.
- If `dst` is us, answer with a **broadcast** IDENT after 0.1–1 s of jitter (at most one
  per 15 s), so every neighbor lacking our key learns it from the same frame.

When to send:
- **On request:** a node that hears a BEACON from a neighbor whose key it lacks requests that
  neighbor's IDENT after 0.5–5 s of jitter, unless the neighbor's IDENT arrives first. It asks
  the same neighbor at most every 10 minutes (bounded tables: 8 pending, 32 recently asked).
- **At a low rate:** one broadcast 1–15 s after start, then every `identIntervalMs` (default
  1 h, +0–10 % jitter), stretched by `util / utilTarget` on a busy channel. `0` = only on request.
- IDENT is own traffic and spends airtime budget (§6.1).

Keys pinned by the host (QR code, key file) override learned ones and are never evicted.
Learned keys are evicted oldest-first when the peer table (`peerCapacity`, default 64) is full.

---

## 5. Routing

### 5.1 Location knowledge

Each node keeps a **location table**: `node ID → (position, observation time, source)`.
It learns from:

1. Beacons (direct, freshest).
2. Beacon gossip entries (epidemic, gradually spreads positions across the mesh).
3. `src_pos` / `dst_pos` / `fwd_pos` fields of any frame it overhears.

When forwarding a frame, a node whose table holds a **fresher** position for `dst` than
the frame's `dst_pos` **rewrites `dst_pos`**. Stale targets get corrected in flight
as they approach the area.

### 5.2 Contention-based geographic forwarding (GEO mode)

GhostWave uses *receiver-based* geographic forwarding. Senders don't pick a next hop from a
neighbor table that might be stale. They transmit **anycast** (`next_hop = 0`), and the
receivers decide among themselves:

1. Transmitter T sends the frame with `fwd_pos = T's position`.
2. Every receiver R computes **progress** `p = d(fwd_pos, dst_pos) − d(R, dst_pos)`.
3. If `p < 150 m`, R stays silent.
4. **Forwarding area.** If T has ≥ 10 fresh neighbors, the first attempt is *narrow*:
   only receivers inside a circle of diameter D = 2.5 km, starting at T and pointing at the
   target, may contend. Any two such nodes are close enough to hear each other, so the
   hidden-terminal problem can't fork the packet into parallel chains. Retries, and any
   sender with fewer neighbors, set the `wide` bit and drop this restriction.
5. R computes a **score** `s = 1 − clamp(p · q / R_max, 0, 1)`. Here
   `q = clamp((RSSI − sensitivity)/10 dB, 0.2, 1)` penalizes marginal links, and `R_max` is the
   preset's *maximum* range (6 km for Trail). Normalizing by typical range saturates the score
   for masted nodes, so every candidate fires at once.
6. R schedules its forward after `(s·K + W)·airtime(frame) + U(0, 120 ms)`, with K = 3.
   **Destination-first window:** W = K + 1 when the target is within 0.4·R_max of T
   (it probably heard T directly, so let it ack first), otherwise W = 0.
7. Before transmitting, R does **CAD**. R **cancels** if it hears *any* other node forward the
   frame first, even one with less progress. The first forwarder has custody, and a second
   one would fork the chain.
8. T treats hearing a forward with progress ≥ 75 m, a `LINK_ACK`, or the E2E ACK as an
   **implicit ack**. Without one it retries: up to 3 tries if it knows a closer neighbor,
   otherwise 2, and a failed narrow attempt always gets one wide retry. After that, it
   moves the frame to its **DTN store** (§5.4).
9. A custodian that hears its upstream *retrying* (because the implicit ack was lost) sends
   a `LINK_ACK` so the upstream stops. The final recipient of a GEO frame does the same.
   Candidates that overhear that `LINK_ACK` cancel.

Why this beats both incumbents for mobile nodes:
- No neighbor table needs to be correct at send time. Whoever is *actually* in range and
  closer right now takes it.
- No learned path can go stale (MeshCore's direct-path failure mode).
- One transmission per hop instead of one per node (Meshtastic flood).

### 5.3 SPRAY mode (destination location unknown)

If the source has no position for `dst`, it uses **binary spray-and-wait** with L = 8 copies.

- Holder with `c > 1` copies meets a neighbor (via its beacon) not in its `given` set
  → sends a `UNICAST` frame carrying `⌊c/2⌋` copies. After the neighbor's `LINK_ACK`, the holder keeps `⌈c/2⌉`.
- Holder with `c = 1` only delivers directly to `dst`.
- **Any holder that learns `dst`'s position switches the message to GEO mode.** Spray finds
  the right area, then geo routing takes it the rest of the way.

### 5.4 DTN store (store-carry-forward)

Each node has a **custody store** (firmware: 64 entries × ≤ 200 bytes = 12.8 KB).

- Entries enter on: GEO void (no progress after retries), SPRAY custody, UNICAST receipt.
- **Re-attempt triggers:**
  - a beacon from a neighbor closer to `dst_pos` by ≥ 150 m,
  - a beacon from `dst` itself (→ UNICAST direct delivery),
  - learning `dst`'s position (SPRAY → GEO),
  - own movement ≥ 250 m (re-check the neighbor table).
- Min 30 s between attempts per entry.
- **Purge** on overheard E2E `ACK`, on TTL expiry (default 6 h, SOS 24 h), or on eviction
  (lowest priority first, then oldest).
- The `dtn_summary` Bloom filter in beacons lets neighbors skip offering messages the other
  side already carries.

### 5.5 End-to-end ACK

`dst` replies with an `ACK` (40 bytes) in GEO mode toward the data's `src_pos`. Any node
that overhears the ACK purges its copy of that message from its store and pending queues.
ACKs are not stored in DTN.

### 5.6 Loops and limits

- `hops_left` starts at 15.
- GEO progress is strictly positive per hop, so loops can only form if `dst_pos` is rewritten
  mid-flight. `hops_left` bounds that case.
- Dedup cache: 256 entries of `(src, msg_id)` with a 1 h lifetime.

---

## 6. Channel access

**CSMA with CAD:** before every transmission, run channel-activity detection. If the channel is
busy, back off for a random 50–600 ms. Priority frames go first in the queue.

### 6.0 Why not GPS-timed slots (tested and rejected for v1)

v0.1 of this doc proposed slotted ALOHA aligned to GPS time, with every 8th slot reserved for
SOS. In simulation it was **worse than CSMA in every scenario**:

- trail: +36% airtime per delivered message
- busy: 37% delivered within 60 s, against 99.7% with CSMA

Slotted ALOHA beats *pure* ALOHA. But LoRa CAD gives real carrier sense, and lining every
start up within a 40 ms window throws that away. GPS time remains available for a future
SOS "quiet window" scheme, which needs its own evaluation.

### 6.1 Airtime budget

Every node runs a token bucket on the traffic it **originates** (its messages, ACKs and
beacons). Relayed traffic is exempt, because throttling relays throttles everyone else's
messages:

- rate = `max(2 %, 30 % / (fresh_neighbors + 1))` of airtime
- depth = 8 s of airtime
- regional duty-cycle cap enforced on top (EU 869.525: 10 %)
- **SOS frames bypass the fair-share bucket** but never the regulatory cap.

The simulation's scenarios have no misbehaving or chatty nodes, so the budget showed no
measurable effect either way. It stays as a fairness guard, but it is **not yet a proven
benefit**.

### 6.2 Adaptive beaconing

- Beacon when moved ≥ 250 m, or every 15 min. Never more often than every 60 s.
- Both intervals stretch by `measured_util / 25 %` when the channel is busier than 25 %.
- Position traffic is **1-hop** only. Positions spread by gossip, not by flooding.

---

## 7. Reliability summary

| Situation | What happens |
|---|---|
| Connected path exists | GEO forwarding, ~1 transmission per hop, E2E ACK |
| Path breaks mid-flight | Custodian stores it, re-attempts when topology changes |
| Destination moved | Fresher `dst_pos` rewritten en route; DTN catches the rest |
| Destination location unknown | Spray-and-wait until someone knows where it is, then GEO |
| Network partitioned | Carried by moving nodes until the partition heals |

---

## 8. SOS lane

- `SOS` packet: type 0x06, priority 2, mode SCOPED_FLOOD, hops 15, `dst` = broadcast,
  **never encrypted** (anyone in range must be able to help). Position + age travel in
  `src_pos`. Payload: `seq` (2), accuracy (1, 4 m steps, 0xFF unknown), battery % (1), free text
  (≤ 60 B).
- The originator re-sends every 5 min with a fresh position, an incrementing `seq` and a fresh
  `msg_id`. A newer `seq` supersedes older ones in every store (one entry per originator).
- **Scoped flood:** a node that hears a new `seq` rebroadcasts it once after a random
  0–2 airtimes + 120 ms, and cancels if it hears the same `seq` twice meanwhile. Relays
  re-flood a *superseding* `seq` at most every 15 min per originator (they still store each
  one). In simulation, re-flooding every 5-min update tripled SOS airtime with no gain in reach.
- Every node that hears an SOS **stores it for 24 h** and offers it to every new neighbor it
  meets (one not heard for 20 min) whose beacon Bloom filter lacks that `(src, msg_id)`. An offer
  restarts the scoped flood (hops 15) in the neighbor's partition, after jitter, and is
  cancelled if another holder offers first. A beacon carries `carrying_sos`. A node that sees it
  from a new neighbor beacons early (0.2–2 s later, ≥ 10 s since its last beacon), so the
  carrier gets the Bloom filter it needs without waiting up to 15 min.
- SOS jumps every transmit queue and bypasses fair-share budgets.
- Any node marked `responder` (or a gateway) replies with `SOS_ACK` (type 0x07, scoped flood,
  payload = acked `seq`) to each new `seq`. Phones show a big "help acknowledged by X" banner.
  `SOS_ACK` is flooded but not stored.
- Rate limit: a node may originate at most 1 SOS per 60 s, so nobody can wedge the
  channel accidentally.

---

## 9. Security

### 9.1 v1 (prototype)

- **DMs:** X25519 ECDH between the Ed25519-derived keys, then a KDF, then ChaCha20-Poly1305
  (RFC 8439, 16-byte tag replacing the 4-byte MIC). The KDF is
  `BLAKE2b-256(shared || min(pubA, pubB) || max(pubA, pubB) || "ghostwave-dm-v1")`, not
  HKDF, because the Monocypher core has BLAKE2b but no SHA-256/HKDF.
  Our X25519 secret is the first 32 bytes of `SHA-512(Ed25519 seed)` (the RFC 8032 signing
  scalar before clamping, which X25519 clamps the same way); a peer's X25519 public key is
  the Edwards→Montgomery map of its Ed25519 key (`crypto_eddsa_to_x25519`). Both sides
  derive the same DM key. Peer keys come from IDENT (§4.5, trust on first use) or are pinned
  by the host.
  The nonce is `src (4 LE) || msg_id (4 LE) || 0 (4)`.
  The associated data is the 19-byte header with byte 2, `prev_hop`, `next_hop`, the mode
  bits and the `has_fwd_pos` / `has_dst_pos` flags zeroed, followed by `src_pos` + age when present.
- **Channels:** pre-shared 256-bit key, same AEAD. The sender uses the DM key if it has the
  peer's key, otherwise the channel key. The receiver tries the DM key, then the channel key.
  Relays forward ciphertext they can't read. A frame that fails authentication is dropped
  without touching the dedup cache, so a forgery can't block the real frame.
  `requireEncryption` drops plaintext DATA.
- Only DATA is encrypted. ACK, LINK_ACK, BEACON and SOS keep the CRC32-C MIC. IDENT is
  signed (Ed25519) instead.
- **Beacons:** 4-byte truncated MAC under the channel key (cheap; prevents casual spoofing,
  not a strong guarantee).
- **Replay:** dedup cache plus the per-source msg_id window.
- Library: Monocypher 4.0.2 core plus its optional SHA-512/Ed25519 add-on
  (`monocypher-ed25519`), vendored and pinned in `firmware/third_party/monocypher` (hashes in
  its README), used on both MCU and Linux. The add-on is used instead of Monocypher's default
  EdDSA (BLAKE2b-based) so signatures are plain RFC 8032 Ed25519 that any library can check;
  the host tests include the RFC 8032 §7.1 vector. The RFC 8439 test vector in the host tests
  checks that it interoperates with libsodium's `crypto_aead_chacha20poly1305_ietf`.
- The nonce is unique only while `(src, msg_id)` is. `msg_id` is random 32-bit, so collisions
  become likely after ~65 k messages per source and key. Make it a persisted counter before v1.0.

### 9.2 Known weaknesses in v1

- Node IDs are 32 bits, so finding a key whose ID collides with a chosen victim's takes
  ~2^32 Ed25519 key generations (hours on a GPU). IDENT's trust on first use stops this from
  replacing a key a node already holds (it raises a key-change warning instead), but a node that
  first hears the attacker's IDENT, or has evicted the victim's learned key, will trust the
  attacker. Pin keys (QR code) for contacts that matter; a longer ID is a v2 question.
- IDENT names carry no timestamp, so an old signed IDENT can be replayed to revert a peer's
  name (never its key).

- `fwd_pos`/`dst_pos` are unauthenticated hop-to-hop, so a malicious node can attract or
  blackhole traffic (true of every LoRa mesh today). Mitigation planned: neighbor
  reputation from implicit-ack success rate.
- Location gossip leaks positions to everyone on the channel. A per-node **privacy mode**
  should publish only a coarse (1 km) position and accept spray-only delivery.
- E2E `ACK`, `LINK_ACK` and `SOS_ACK` carry only the CRC MIC, so a forged ACK can purge a
  message from stores and a forged SOS_ACK can show a false "help acknowledged". A truncated
  MAC under the DM or channel key is the planned fix (as for beacons).

### 9.3 v2

Sender-key group rotation (remove a lost device from a channel), and a double-ratchet
for DMs once a session exists.

---

## 10. Client protocol (phone ↔ node)

- Transports: BLE (Nordic UART-style service), USB serial, TCP (Pi daemon).
- Framing: `0x94 0xC3 len_lo len_hi` + protobuf `ToRadio`/`FromRadio`. This is the same
  framing idea as Meshtastic, so existing serial tooling is easy to adapt, but the schema
  is ours.
- Published as `proto/ghostwave.proto` with its own version number.

---

## 11. Bridge (v1.x)

A Pi with two radios (or one radio + MQTT) translates:
- GhostWave DATA ↔ Meshtastic TEXT_MESSAGE_APP on a configured channel
- GhostWave DATA ↔ MeshCore channel/DM via a companion-protocol link

Identity mapping is explicit (the user pairs contacts). The bridge never forwards SOS
*into* other meshes without user opt-in.

---

## 12. Regulatory

- US 902–928 MHz: Part 15.247 allows up to 1 W only for frequency hopping (≥ 50
  channels when the 20 dB BW is < 250 kHz) or for digital modulation with a 6 dB BW
  ≥ 500 kHz. A single fixed BW125 channel meets neither test. High-power fixed-channel
  LoRa is a grey area that existing hobby projects live in; otherwise you fall back to
  Part 15.249's much lower field-strength limit. **Action item: have a compliance
  consultant choose the rule path (likely a hopping or hybrid mode) before selling
  hardware.** Firmware must cap the E22P's 30 dBm output per preset.
- EU 869.4–869.65 MHz: 500 mW ERP, 10 % duty cycle. Enforced in firmware.

---

## 13. Open questions

1. `R_max` and the forwarding-area diameter D per preset: measure them in the field. Make them
   adaptive (for example, D from the median distance at which neighbors still hear each other).
2. Privacy vs. routing: how coarse can positions be before GEO stops working? Test in simulation.
3. Spray copy count L vs. network size. Test in simulation.
4. The forwarding area helps dense meshes a lot but costs sparse ones some latency (§14).
   The 10-neighbor gate is a first guess.
5. Bridge identity model.
6. Not yet implemented in firmware: velocity in beacons, client protocol, the
   regulatory duty-cycle cap, the beacon MAC (§9.1), Bloom-filter use on receive for DTN
   *data* (it is used for SOS), and IDENT in the Pi daemon (it still loads an X25519 secret
   and peer keys from files). Implemented since v0.1: ChaCha20-Poly1305 AEAD for DATA (DM +
   channel keys), the SOS lane (§8), IDENT (§4.5) and Ed25519 → X25519 secret-key conversion.
7. SOS cost: in backcountry with 6 concurrent SOS events the lane spends ~3× flood3's SOS
   airtime, because every node holds and re-offers every SOS. Its reach to fixed nodes beats flood3 in the
   tail but not flood7 (§14). Worth trying: coarser (per-15-min) Bloom keys for updates,
   and checking whether GhostWave's higher channel load causes the flood7 gap.

---

## 14. Simulation results (sim/ghostsim.py, 3 seeds each)

Same mobility, traffic and radio model for every protocol: SF9/BW125, path-loss exponent 3.5,
5 dB fading, capture, half-duplex, CAD. Baselines are *simplified models* of Meshtastic
(managed flood) and MeshCore (repeater flood + learned paths), not their firmware.

| Scenario | Protocol | Delivered | ≤ 60 s | Median latency | Airtime / delivered msg |
|---|---|---|---|---|---|
| **trail** (8 km, 50 nodes) | flood (hop 3) | 99.9 % | 99.9 % | 1.0 s | 21.9 s |
| | path-learning | 100 % | 100 % | 0.8 s | **5.4 s** |
| | **GhostWave** | 100 % | 99.8 % | 1.0 s | 5.7 s |
| **backcountry** (25 km, 40 nodes) | flood (hop 3) | 45.9 % | 44.3 % | 1.8 s | 25.5 s |
| | flood (hop 7) | 62.9 % | 60.2 % | 2.5 s | 30.2 s |
| | path-learning | 21.0 % | 21.0 % | 0.9 s | 7.8 s |
| | **GhostWave** | **98.7 %** | 54.4 % | 26 s | 25.7 s |
| **busy** (5 km, 120 nodes) | flood (hop 3) | 90.9 % | 11.4 % | 419 s | 22.0 s |
| | path-learning | 95.7 % | 50.5 % | 54 s | 10.1 s |
| | **GhostWave** | **100 %** | **99.7 %** | **1.4 s** | **4.2 s** |

Ablations (which pillar does the work):

| Variant | trail air/msg | backcountry delivered | busy ≤ 60 s |
|---|---|---|---|
| GhostWave | 5.7 s | 98.7 % | 99.7 % |
| without DTN | 5.7 s | **49.1 %** | 99.9 % |
| without forwarding area | 6.8 s | 98.3 % (≤60 s: 64.5 %) | **85.2 %** |
| without airtime budget | 5.7 s | 97.9 % | 99.9 % |
| with GPS slots | 7.8 s | 98.3 % | **37.3 %** |

**Takeaways**
- **DTN is the backcountry win.** It doubles delivery in partitioned terrain. The price: about half
  of those messages arrive after a minute or more, because someone had to carry them.
- **Duplicate-free geographic forwarding is the busy-mesh win.** It uses 5× less airtime than flooding
  and 2.4× less than path-learning, with sub-2 s latency, while flooding collapses.
- In a well-connected mesh with good fixed repeaters, **MeshCore-style path-learning is
  already excellent**. GhostWave matches it there. It doesn't beat it.
- GPS slots were rejected (§6.0). The airtime budget is unproven (§6.1).

### SOS reach (`--sos`, backcountry, 10 seeds × 6 SOS events)

Each SOS starts at a random mobile node at a random time and is re-sent every 5 min until the end
of the run. Metric: time until **any fixed node** holds it. flood* sends it as a plain hop-limited
flood. Results in `sim/results/backcountry.sos.txt`.

| Protocol | Reached | ≤ 60 s | Median | p90 | Worst | SOS airtime / run |
|---|---|---|---|---|---|---|
| flood (hop 3) | 100 % | 83 % | 1 s | 301 s | 3001 s | 455 s |
| flood (hop 7) | 100 % | 92 % | 1 s | 5 s | 2101 s | 769 s |
| **GhostWave** | 100 % | 87 % | 1 s | 131 s | **1501 s** | 1480 s |

Most SOS events reach a fixed node within seconds whatever the protocol. Against flood3, the epidemic store
halves the p90 and the worst case. flood7's wider instant flood still wins the p90 at half the
airtime, so the SOS lane is **not yet a clear win** on this metric (§13 item 7). Its
other job is getting the SOS to every node and keeping it there for 24 h. This metric doesn't
measure that.
