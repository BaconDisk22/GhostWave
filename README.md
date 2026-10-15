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
firmware/pi/           ghostwaved: Raspberry Pi daemon for the Backcountry Signal HAT (RadioLib),
                       or with a UDP virtual radio on any Linux/macOS host
tools/vmesh.sh         4-daemon virtual mesh demo: encrypted DM and SOS across a partition
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
cd firmware && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

`ghostwave_tests` covers the core; `ghostwave_vchannel_tests` covers the virtual radio's channel model.

## Virtual mesh (no hardware)

`ghostwaved --radio udp` swaps the E22P for a virtual radio: every daemon on a UDP multicast group
(default `239.77.0.1:47700`, TTL 1) hears every frame, and each receiver decides whether it would
have decoded it over the air. It is the same channel model as `sim/ghostsim.py`: log-distance path
loss (31.7 dB at 1 m, exponent 3.5), 5 dB Gaussian fading per frame, −129 dBm sensitivity, 6 dB
capture, and half duplex. Frames between −135 and −129 dBm cannot be decoded but still collide.
The derived RSSI goes to the router. `channelBusy` (CSMA) is true while a decodable frame's
airtime (`airtimeMs`, SF9/BW125) overlaps now, or while we are sending.

Each datagram carries the sender's position and EIRP, so position must come from `--lat/--lon` or
`--nmea FILE`. The file can be a plain file or a FIFO of NMEA sentences, and it is followed like
`tail -f`, so appending `$GPGGA` lines moves the node. Datagrams are numbered per sender: macOS
loops multicast back on more than one path, so one frame can arrive twice, and the copies would
otherwise collide with each other.

Build it on Linux or macOS. It needs no lgpio or RadioLib:

```bash
cd firmware && cmake -S . -B build-virt -DGHOSTWAVE_BUILD_VIRT=ON && cmake --build build-virt
./build-virt/ghostwaved --id a1 --key /tmp/a1.key --lat 46.50 --lon -114.0
./build-virt/ghostwaved --id b2 --key /tmp/b2.key --lat 46.52 --lon -114.0 --vlog   # ~2.2 km north
```

The stdin commands are the same as on the Pi. `stats` adds a `vradio` line with frames heard,
decoded, out of range, too weak, and collided. `--vlog` prints one line per frame heard, with its
distance and RSSI. Other options: `--mcast GROUP:PORT` (a separate mesh), `--vif ADDR` (the
interface to use, e.g. `127.0.0.1` on a host with no LAN), `--vtx-dbm` (EIRP, default 24 =
22 dBm + 2 dBi), `--vgain` (receive antenna gain, default 2 dBi), and `--vfade 0` (no fading).
Daemons on several machines on one LAN form one mesh. Any host on that LAN can join the group,
so treat it as a test bench.

`tools/vmesh.sh` runs the whole story in about five minutes. Four daemons form a chain, A (0 km),
B (2 km), C (9 km) and D (11 km), with ~3.3 km of range. That makes two partitions. A sends D an
encrypted DM and raises an SOS. B is a responder: it acks the SOS and carries both. It then walks
north to 7.5 km (fake NMEA), meets C, and the DM and SOS reach D. B then walks back, so D's
end-to-end ACK can reach A. That step is best effort and the script does not check it.
A sniffer on the group checks that the DM text never went over the air in clear.

```bash
tools/vmesh.sh            # builds firmware/build-virt if needed; --keep keeps logs and keys
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
   - `peer <hex-id> <hex-pubkey>` adds a peer key without a restart.
   - `sos <text>` starts an SOS, repeated every 5 minutes with the current position. `sosstop` ends it.
   - `sosack <hex-id>` tells that node its SOS was heard.

### Keys

- **Identity key** (`--key`, default `/var/lib/ghostwave/identity.key`). This is an X25519 secret
  stored as 64 hex characters. It is created on first run with mode 600, in a mode-700 directory.
  The public half is printed at startup and by `keys`.
- **Peers** (`--peers FILE`). One `<hex-id> <hex-pubkey>` per line; `#` starts a comment.
  Messages to a listed peer are sealed with a DM key, which only the two of you can open.
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
   `ghostwaved node <id> ...` and `key <pubkey>, ...`.
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

**Verified so far:** the core builds and all 99 host tests pass, plus 32 for the virtual radio's channel
model. `tools/vmesh.sh` passes on macOS: the DM and SOS cross the partition, and the DM text never
goes over the air in clear. `ghostwaved` compiles and links
against RadioLib 7.8.1 on macOS, using a stub `lgpio` (no lgpio for macOS). Key-file handling was run
against that build: create with mode 600, reload, bad and missing files, and peers-file parsing.
**Not verified yet:** running on real hardware, including the bench test above.
