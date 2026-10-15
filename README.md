# GhostWave

A LoRa mesh for people who move through places with no coverage. It is built on four ideas:

1. **Delay-tolerant delivery.** If no path exists, the message is carried until one does.
2. **Geographic forwarding.** Packets head toward where the destination *is*, so moving nodes
   never break a stored route.
3. **Congestion control.** One transmission per hop, 1-hop beacons, and an airtime budget.
4. **SOS lane.** Scoped flood, plus a 24 h epidemic store that carriers hand to every new neighbor.

DATA payloads are end-to-end encrypted with ChaCha20-Poly1305 (X25519 DM keys or a channel PSK).

Status: **v0.1 prototype**. The design is in [docs/DESIGN.md](docs/DESIGN.md). Simulation results are in
[DESIGN §14](docs/DESIGN.md#14-simulation-results-simghostsimpy-3-seeds-each).

## Headline results (simulation)

| | Meshtastic-like flood | MeshCore-like path-learning | GhostWave |
|---|---|---|---|
| Backcountry: 25 km, 40 nodes, mostly hikers | 46–63 % delivered | 21 % delivered | **98.7 % delivered** |
| Busy: 5 km, 120 nodes | 11 % within 60 s | 51 % within 60 s | **99.7 % within 60 s** |
| Trail: 8 km, 50 nodes, good repeaters | 21.9 s air/msg | **5.4 s** | 5.7 s |

The baselines are simplified models, not the real firmware. Read the takeaways and caveats in
DESIGN §14 before quoting these numbers.

## Layout

```
docs/DESIGN.md         protocol spec v0.1 (packet format, routing, DTN, budgets, SOS, security)
sim/ghostsim.py        event-driven simulator: mobility, LoRa airtime, collisions, 3 protocols
sim/results/           raw output of the benchmark runs
firmware/core/         portable C++17 protocol core (packet codec + AEAD, router, SOS, NMEA, airtime)
firmware/tests/        host tests, including multi-node runs over a simulated radio
firmware/third_party/  Monocypher 4.0.2 (vendored, pinned; BSD-2 / CC0)
firmware/pi/           ghostwaved: Raspberry Pi daemon for the Backcountry Signal HAT (RadioLib)
firmware/mcu/          PlatformIO firmware for ESP32-S3 Heltec boards (Wireless Tracker V2, V3)
```

## Run the simulator

```bash
cd sim && python3 ghostsim.py --scenario trail --seeds 1
```

Full benchmark (about 15 min; the busy scenario is slow): `python3 ghostsim.py --markdown`.
Ablations: `--proto ghostwave ghostwave-nodtn ghostwave-noarea ghostwave-nobudget ghostwave-slots`.
SOS reach to fixed nodes: `python3 ghostsim.py --scenario backcountry --proto flood3 ghostwave --sos`.

## Build and test the core (any machine)

```bash
cd firmware && cmake -S . -B build && cmake --build build && ./build/ghostwave_tests
```

## Run on the Pi HAT

Pins are taken from the HAT: CS BCM8 (CE0), DIO1 BCM6, BUSY BCM5, RESET BCM18, module EN BCM26.

1. In `raspi-config`: enable **SPI**. Enable the **serial port** hardware, but **not** the login shell over serial.
2. In `/boot/firmware/config.txt`, add `gpio=26=op,dh`. This holds the E22P's enable line high.
3. Stop anything else that uses the radio: `sudo systemctl disable --now meshtasticd`.
   If openHop/pyMC is installed, stop that too.
4. Install the build tools: `sudo apt install cmake g++ git liblgpio-dev`.
5. Build:
   ```bash
   cd firmware && cmake -S . -B build -DGHOSTWAVE_BUILD_PI=ON -DGHOSTWAVE_BUILD_TESTS=OFF && cmake --build build
   ```
6. Run: `sudo ./build/ghostwaved` (GPS on `/dev/serial0`). For a bench test without a GPS fix, use
   `--lat 46.87 --lon -113.99`.
7. Type commands on stdin:
   - `send <hex-id> hello` sends a message.
   - `known <hex-id> <lat> <lon>` tells the node where a contact is.
   - `stats` and `pos` show counters and the current fix.
   - `keys` prints this node's id and public key, as a line for another node's peers file.
   - `peer [<hex-id>] <hex-pubkey>` pins a peer key without a restart.
   - `ident <hex-id>` asks a neighbor for its IDENT (its signed key and name).
   - `sos <text>` starts an SOS, repeated every 5 minutes with the current position. `sosstop` ends it.
   - `sosack <hex-id>` tells that node its SOS was heard.

### Keys

- **Identity key** (`--key`, default `/var/lib/ghostwave/identity.key`). This is an Ed25519 seed,
  stored as `ed25519:` and 64 hex characters. It is created on first run with mode 600, in a
  mode-700 directory. The node ID is the hash of the public key (DESIGN §3), so `--id` is optional;
  if you pass it, it must match. The public key is printed at startup and by `keys`.
- **Name** (`--name`, up to 9 bytes). Sent in this node's IDENT broadcasts (DESIGN §4.5).
  Nodes that hear an IDENT learn our key on first use and print `IDENT new peer <id> "<name>"`.
- **Old key files.** Before IDENT, the key file held a bare X25519 secret. The daemon now refuses
  such a file. Either move it aside, so a new Ed25519 identity is made (this gives a new node ID,
  so peers must update their peers files), or run with `--legacy-x25519`. That keeps the old key
  and `--id`, and still learns peers from IDENT, but it cannot send one.
- **Peers** (`--peers FILE`). One `[<hex-id>] <hex-pubkey>` per line; `#` starts a comment.
  A bare key is the peer's Ed25519 key. It must hash to the ID, and the ID may be left out.
  For a `--legacy-x25519` node, write `<hex-id> x25519:<hex-pubkey>`, as its `keys` prints.
  Messages to a listed peer are sealed with a DM key, which only the two of you can open.
  A listed key is pinned: it overrides anything learned from IDENT. If a signed IDENT ever
  brings a different key for a known ID, the daemon prints a `KEY CHANGE` warning and keeps
  the old key.
- **Channel key** (`--psk FILE`, optional). 32 bytes, as 64 hex characters or raw.
  Make one with `openssl rand -hex 32`. It seals messages to anyone not in the peers file.
  Every node with the same file can read them.
- With neither a peer key nor a channel key, a message goes out in plaintext with only a CRC.
  `--require-enc` drops plaintext messages addressed to this node. They show in `stats` as `authfail`.
- SOS frames are never encrypted, so any GhostWave node can relay them and act on them (DESIGN §8).
  `--responder` makes the node reply `SOS_ACK` to every new SOS automatically.

### Two-node bench test

You need two Pis with HATs, A and B, a few metres apart. Keep both antennas connected. Leave the
power at the default 10 dBm.

1. **Start each node once to make its key.** On A:
   ```bash
   sudo ./build/ghostwaved --lat 46.8700 --lon -113.9900
   ```
   On B, use `--lat 46.8710 --lon -113.9900`. This is a made-up position 110 m north; the boards can
   still sit on the same bench. Each node prints
   `ghostwaved node <id> ...` and `key ed25519 <pubkey>, ...`. Within a minute each node
   should also print `IDENT new peer <id>` for the other. That key is trusted on first use;
   the next step pins it instead.
2. **Swap public keys.** Type `keys` on each node. Stop both nodes with Ctrl-C. On each Pi, run
   `sudo mkdir -p /etc/ghostwave`. Copy A's `keys` line into `/etc/ghostwave/peers` on B, and B's line
   into the same file on A.
3. **Restart both with the peer keys, refusing plaintext.** Run on A, then the same on B with B's
   position:
   ```bash
   sudo ./build/ghostwaved --lat 46.8700 --lon -113.9900 --peers /etc/ghostwave/peers --require-enc
   ```
   The startup line should say `1 peer key, plaintext DATA refused`.
4. **Encrypted DM.** On A, type `known <B-id> 46.8710 -113.9900`, then `send <B-id> hello over aead`.
   - B should print `MSG from <A-id> ...: hello over aead`.
   - A should print `DELIVERED to <B-id> ...` within a few seconds.
   - `stats` on B should show `authfail 0`.
5. **Plaintext is refused.** Restart A without `--peers` and send again. B prints nothing,
   its `authfail` count goes up, and A gets no `DELIVERED`. Restart A with `--peers` before going on.
6. **SOS.** On A, type `sos bench test, ankle`.
   - A prints `SOS on`.
   - B prints `SOS from <A-id> seq 1 at 46.870000 -113.990000: bench test, ankle`.
7. **SOS ack.** On B, type `sosack <A-id>`. A should print `SOSACK from <B-id> for seq 1`.
   To test automatic acks instead, start B with `--responder`.
8. **Stop the SOS.** On A, type `sosstop`. `stats` on A drops the `SOS-ACTIVE` flag. A can start a new
   SOS after 60 seconds. B keeps holding A's SOS until its TTL runs out (24 h).

Defaults: 914.875 MHz, SF9/BW125, sync word 0x24, **10 dBm SX1262 drive**. The E22P's power
amplifier adds roughly 8–10 dB on top. Check DESIGN §12 before raising the power.

**Verified so far:** the core builds and all host tests pass. `ghostwaved` compiles and links
against RadioLib 7.8.1 on macOS, using a stub `lgpio` (no lgpio for macOS). Key-file handling was run
against that build: create with mode 600, reload, bad and missing files, and peers-file parsing.
**Not verified yet:** running on real hardware, including the bench test above.

## Run on a Heltec board (ESP32-S3)

`firmware/mcu/` is a PlatformIO project (Arduino framework, RadioLib 7.8.1). It runs the same router
as `ghostwaved`. Supported boards:

| env | Board | Position |
|---|---|---|
| `heltec_wireless_tracker_v2` | Heltec Wireless Tracker V2 (SX1262 + KCT8103L PA/LNA + UC6580 GPS) | GPS, or a stored position until the first fix |
| `heltec_v3` | Heltec WiFi LoRa 32 V3 (SX1262, no GPS) | stored position (`setpos`) |

The pin maps are in `firmware/mcu/src/board.h`. They come from Meshtastic's variant files, and each
value cites its source. The nRF52 boards (T096, RAK4631, T1000-E) will be a separate port.

### Flash

1. Install PlatformIO Core: `pip install platformio` (CI uses 6.2.0).
2. Plug the board in over USB. Connect the LoRa antenna first: transmitting without one can damage
   the radio.
3. Build and flash. This erases Meshtastic/MeshCore; you can reflash those later from their web
   flashers.
   ```bash
   cd firmware/mcu && pio run -e heltec_wireless_tracker_v2 -t upload
   ```
   Use `-e heltec_v3` for the V3. If the upload cannot find the port, hold **BOOT**, tap **RST**,
   release **BOOT**, and run the command again. On the Tracker, press **RST** after the upload.
4. Open the console: `pio device monitor` (115200 baud). It echoes what you type and sends on Enter.

On first boot the node creates a node ID (from the chip's MAC) and an X25519 identity key (from the
hardware RNG), and stores them in NVS. Like `ghostwaved`, the board does not have an Ed25519 identity
yet. It learns peer keys from IDENT (it prints `IDENT learned key for ...`) but does not send IDENT. They survive reflashing the app. `pio run -t erase` wipes them.

### CLI

The commands match `ghostwaved`:

- `send <hex-id> <text>`, `known <hex-id> <lat> <lon>`, `pos`, `keys`, `peer <hex-id> <hex-pubkey>`.
- `sos <text>`, `sosstop`, `sosack <hex-id>`.
- `stats` prints the same counters as on the Pi. `status` adds the board, frequency, power, table use
  against each cap, free and minimum free heap, and the key settings.

These replace `ghostwaved`'s command-line options. They are saved in NVS:

| Command | Effect |
|---|---|
| `peer <hex-id> <hex-pubkey>` | Adds a DM key now and saves it (up to 32 peers). |
| `setpos <lat> <lon>` / `setpos clear` | Stored position. Used until a GPS fix arrives. Required on the V3. |
| `psk <64 hex>` / `psk gen` / `psk off` | Channel key. `gen` makes one and prints it once, so you can paste it on the other nodes. `off` applies after a reboot. |
| `power <dBm>` | Output at the antenna port. Default 20, capped at 22 (see below). Applies now. |
| `freq <MHz>` | 902.0625–927.9375. Default 914.875. Applies after `reboot`. |
| `requireenc on\|off` | Same as `--require-enc`. Applies after `reboot`. |
| `responder on\|off` | Same as `--responder`. Applies after `reboot`. |
| `reboot` | Restarts the node. |

**Radio:** the "Trail" preset, the same as `ghostwaved`: SF9, BW125, CR4/5, 16-symbol preamble, sync
word 0x24, CRC on. Channel-busy checks use CAD. **Power:** `power` sets the level at the antenna port.
On the Tracker V2, the firmware subtracts the KCT8103L front end's gain before setting the SX1262. It
uses Meshtastic's gain table, which we have not measured. Every board is capped at 22 dBm, the most a
bare SX1262 can do, until a rule path is chosen (DESIGN §12). Only US 902–928 MHz is supported; the EU
duty-cycle limiter is not ported yet.

**Memory:** the router's tables are bounded. The store holds 64 messages and the SOS table 16
originators. The duplicate-suppression table holds 512 entries, and the neighbor and location tables
512 each. At boot the node prints its free heap next to a worst-case estimate for all tables full.
That estimate is about 180 KB; it costs every stored frame at the full 128 bytes. The ESP32-S3 has
320 KB of data RAM, and static data uses about 22 KB of it. The free heap at boot has not been measured
on hardware yet. If it is too small, the boot line ends with `WARNING: may not fit`. `status` shows how
full each table is and the minimum free heap seen since boot.

### Two-board over-the-air test (encrypted DM + SOS)

You need two flashed boards, A and B, a couple of metres apart, each with an antenna. At bench
distance, turn the power down on both with `power 2`. Two V3s work, and so does one Tracker with one
V3.

1. **Give each a position.** On A, type `setpos 46.8700 -113.9900`. On B, type
   `setpos 46.8710 -113.9900` (made up, 110 m north). A Tracker with a GPS fix can skip this.
   Then type `pos` on each; it should print `fix ...`.
2. **Swap public keys.** Type `keys` on each board. It prints `<id> <pubkey>`. On B, type
   `peer ` followed by A's line. On A, type `peer ` followed by B's line. Each prints `ok`.
3. **Refuse plaintext.** On both boards, type `requireenc on`, then `reboot`. The startup lines should
   say `1 peer key, plaintext DATA refused`.
4. **Encrypted DM.** On A, type `known <B-id> 46.8710 -113.9900`, then `send <B-id> hello over aead`.
   - B should print `MSG from <A-id> ...: hello over aead`.
   - A should print `DELIVERED to <B-id> ...` within a few seconds.
   - `stats` on B should show `authfail 0`, and `tx`/`rx` counts on both.
5. **SOS.** On A, type `sos bench test, ankle`.
   - A prints `SOS on`.
   - B prints `SOS from <A-id> seq 1 at 46.870000 -113.990000: bench test, ankle`.
6. **SOS ack.** On B, type `sosack <A-id>`. A should print `SOSACK from <B-id> for seq 1`.
7. **Stop the SOS.** On A, type `sosstop`. `stats` drops the `SOS-ACTIVE` flag.
8. **Check memory.** Type `status` on both boards. Every table should be under its cap, and `min free`
   should stay well above zero.

To test against a Pi, run `ghostwaved` with its defaults (914.875 MHz). Swap keys the same way:
paste the Pi's `keys` line into `peer` on the board, and the board's line into the Pi's peers file.

**Verified so far (MCU):** both envs compile and link with `pio run`. **Not verified yet:** anything
on hardware. That includes the pin maps, the FEM sequencing, GPS parsing from the UC6580, and the test
above.
