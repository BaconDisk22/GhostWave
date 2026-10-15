# Ridgeline (working name)

A LoRa mesh for people who move through places with no coverage. It is built on four ideas:

1. **Delay-tolerant delivery.** If no path exists, the message is carried until one does.
2. **Geographic forwarding.** Packets head toward where the destination *is*, so moving nodes
   never break a stored route.
3. **Congestion control.** One transmission per hop, 1-hop beacons, and an airtime budget.
4. **SOS lane.** Scoped flood, plus a 24 h epidemic store that carriers hand to every new neighbor.

DATA payloads are end-to-end encrypted with ChaCha20-Poly1305 (X25519 DM keys or a channel PSK).

Status: **v0.1 prototype**. The design is in [docs/DESIGN.md](docs/DESIGN.md). Simulation results are in
[DESIGN §14](docs/DESIGN.md#14-simulation-results-simridgesimpy-3-seeds-each).

## Headline results (simulation)

| | Meshtastic-like flood | MeshCore-like path-learning | Ridgeline |
|---|---|---|---|
| Backcountry: 25 km, 40 nodes, mostly hikers | 46–63 % delivered | 21 % delivered | **98.7 % delivered** |
| Busy: 5 km, 120 nodes | 11 % within 60 s | 51 % within 60 s | **99.7 % within 60 s** |
| Trail: 8 km, 50 nodes, good repeaters | 21.9 s air/msg | **5.4 s** | 5.7 s |

The baselines are simplified models, not the real firmware. Read the takeaways and caveats in
DESIGN §14 before quoting these numbers.

## Layout

```
docs/DESIGN.md         protocol spec v0.1 (packet format, routing, DTN, budgets, SOS, security)
sim/ridgesim.py        event-driven simulator: mobility, LoRa airtime, collisions, 3 protocols
sim/results/           raw output of the benchmark runs
firmware/core/         portable C++17 protocol core (packet codec + AEAD, router, SOS, NMEA, airtime)
firmware/tests/        host tests, including multi-node runs over a simulated radio
firmware/third_party/  Monocypher 4.0.2 (vendored, pinned; BSD-2 / CC0)
firmware/pi/           ridgelined: Raspberry Pi daemon for the Backcountry Signal HAT (RadioLib)
```

## Run the simulator

```bash
cd sim && python3 ridgesim.py --scenario trail --seeds 1
```

Full benchmark (about 15 min; the busy scenario is slow): `python3 ridgesim.py --markdown`.
Ablations: `--proto ridgeline ridgeline-nodtn ridgeline-noarea ridgeline-nobudget ridgeline-slots`.
SOS reach to fixed nodes: `python3 ridgesim.py --scenario backcountry --proto flood3 ridgeline --sos`.

## Build and test the core (any machine)

```bash
cd firmware && cmake -S . -B build && cmake --build build && ./build/ridgeline_tests
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
   cd firmware && cmake -S . -B build -DRIDGELINE_BUILD_PI=ON -DRIDGELINE_BUILD_TESTS=OFF && cmake --build build
   ```
6. Run: `sudo ./build/ridgelined` (GPS on `/dev/serial0`). For a bench test without a GPS fix, use
   `--lat 46.87 --lon -113.99`.
7. Type commands on stdin:
   - `send <hex-id> hello` sends a message.
   - `known <hex-id> <lat> <lon>` tells the node where a contact is.
   - `stats` and `pos` show counters and the current fix.

Defaults: 914.875 MHz, SF9/BW125, sync word 0x24, **10 dBm SX1262 drive**. The E22P's power
amplifier adds roughly 8–10 dB on top. Check DESIGN §12 before raising the power.

**Verified so far:** the core builds and all 46 host tests pass, and `ridgelined` compiles against
RadioLib 7.8.1. **Not verified yet:** running on real hardware.
