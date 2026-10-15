# Ridgeline on ESP32-S3 dev boards

PlatformIO project (Arduino framework, RadioLib 7.8.1) that runs the portable core from
`firmware/core` on off-the-shelf LoRa boards. It speaks the same line commands as `ridgelined`,
over USB serial. It also uses the same PHY, frequency and sync word, so a dev board and a Pi node
can talk to each other.

| env | board | position |
|---|---|---|
| `heltec_wireless_tracker_v2` | Heltec Wireless Tracker V2 (ESP32-S3, SX1262, KCT8103L FEM, UC6580 GPS) | onboard GPS, falls back to a stored position |
| `heltec_v3` | Heltec WiFi LoRa 32 V3 (ESP32-S3, SX1262) | stored position (`setpos`) |

Pin maps come from Meshtastic's variant files (`variants/esp32s3/heltec_wireless_tracker_v2/variant.h`
and `variants/esp32s3/heltec_v3/variant.h`, meshtastic/firmware @ 7579bfe). Each value is cited in
[src/board.h](src/board.h). Things that are **not** independently confirmed:

- **Tracker V2 PA gain curve.** We use Meshtastic's `TX_GAIN_LORA` table to convert the requested
  output power into SX1262 drive. Nobody here has measured it.
- **Heltec V3 `Vext` (GPIO 36, active low).** Meshtastic's comment says it powers the OLED "and the
  lora antenna boost". We could not confirm the antenna part from a schematic, so the firmware switches
  it on, as Meshtastic does.
- **Heltec V4, Mesh Node T096, RAK4631, T1000-E.** Not supported yet. The V4 has a different FEM, and
  the nRF52 boards need a separate port.

## Radio settings

- "Trail" preset (DESIGN §2): SF9, BW125, CR 4/5, 16-symbol preamble, explicit header, CRC on.
- Frequency: 914.875 MHz. This is the same as `ridgelined`: inside the US 915 band and clear of the
  Meshtastic/MeshCore defaults. Sync word 0x24.
- Channel access: `channelBusy()` runs a CAD (`scanChannel`) before each transmission.
- **TX power** is set as *conducted output at the antenna port*. The default is 20 dBm and the firmware
  caps it at **22 dBm**. DESIGN §12 leaves the US rule path open: a fixed BW125 channel does not qualify
  for 15.247's 1 W allowance. Until a compliance decision is made, no board exceeds what a bare SX1262
  can do.
  - On the Tracker V2, the KCT8103L adds about 14 dB, so 20 dBm out means about 6 dBm of drive.
  - On the V3, output equals drive.

## Build and flash

```bash
pip install platformio            # or use the VS Code PlatformIO extension
cd firmware/mcu
pio run                           # builds both envs
pio run -e heltec_wireless_tracker_v2 -t upload
pio run -e heltec_v3 -t upload
pio device monitor                # 115200 baud; type `help`
```

- **Tracker V2:** uses native USB, so it shows up as a USB CDC port (303a:1001). If the upload can't
  find it, hold **BOOT** (GPIO 0), tap **RST**, release **BOOT**, then upload again.
- **V3:** uses a CP2102 bridge. Install the Silicon Labs driver if no port appears.

If the board already runs Meshtastic or MeshCore, flashing replaces that firmware. Before you do, save
its config with the Meshtastic app or CLI if you want to go back.

### First boot

- **Node ID:** derived from the chip's MAC address (CRC32-C), so it stays the same across reflashes.
- **Identity key:** a new X25519 key is generated from the hardware RNG, with the SAR ADC entropy source
  enabled.

Both are stored in NVS (Preferences namespace `ridgeline`), along with the channel key, peer keys and
settings. NVS is **not encrypted**: anyone holding the board can read the keys out of flash. `wipe
confirm` erases everything, and the next boot generates a fresh key.

## CLI

| command | |
|---|---|
| `send <hex-id> <text>` | Queue a message. It is encrypted if a peer key or channel key applies. |
| `known <hex-id> <lat> <lon>` | Tell the router where a contact is. |
| `sos <text>` / `sosstop` | Start or stop an SOS. It repeats every 5 min with the current position. |
| `sosack <hex-id>` | Acknowledge a held SOS. |
| `keys` | Print `<id> <pubkey>`, ready to paste into `peer` on another node (or a `ridgelined --peers` file). |
| `peer <hex-id> <hex-pubkey>` | Add a peer key. Up to 16 are saved in NVS. |
| `peers` | List the saved peer keys. |
| `psk <64 hex>` / `psk off` | Set or remove the channel key. Removal takes effect after a reboot. |
| `status` | Node, radio and position details; router counters; table fill levels; free heap. |
| `stats` / `pos` | Same output as `ridgelined`. |
| `setpos <lat> <lon>` / `setpos clear` | Stored position. Used when there is no GPS fix. |
| `power [dBm]` | Show or set the output power. Capped at 22, and saved. |
| `freq <MHz>` | 902–928. Saved. |
| `responder on\|off` | Auto-acknowledge every new SOS. Applies after a reboot. |
| `requireenc on\|off` | Drop plaintext DATA addressed to us. Applies after a reboot. |
| `reboot`, `wipe confirm` | Restart; erase all NVS state. |

## Memory

The router's tables are bounded, so a busy channel cannot exhaust the heap.

Entry sizes below come from `sizeof` under the Xtensa compiler, 32-bit. Each also includes a 16 B
map node and about 8 B of allocator overhead.

| table | cap | per entry, worst case | total |
|---|---|---|---|
| DTN store | 64 msgs | ~610 B: 208 B entry, ≤136 B payload, 8 spray-copy set nodes | ~39 KB |
| location table | 512 | 64 B | 32 KB |
| neighbor table | 512 | 64 B | 32 KB |
| seen / dedupe | 768 (`kSeenCapacity`) | 88 B | ~68 KB |
| SOS | 16 originators | ~1.7 KB: 224 B entry, payload, 32 offer records | ~27 KB |
| DM keys | 64 | 64 B | 4 KB |

That is about 200 KB when every table is full. Static RAM is about 21 KB of the S3's 320 KB, and Wi-Fi
and BT are never started, so this should leave around 100 KB of headroom. That figure is not yet
measured on hardware: record what `status` reports during the bench test.

- `status` prints the free heap, the lowest free heap seen since boot, the largest free block, and each
  table's fill level.
- The firmware warns once if the lowest free heap falls below 24 KB.
- The TX queue and the per-message timers are not capped by count. They drain as frames go out or time
  out.

## Two-board bench test (encrypted DM + SOS)

You need two flashed boards, A and B, on USB with a serial monitor open on each. Keep them at least
2 m apart, or turn power down to about 2 dBm (`power 2`) so the receivers don't saturate. Antennas
must be attached before power-up: transmitting into an open SMA can damage the PA.

1. **Positions.** Geographic forwarding needs a position on the sender.
   - The Tracker V2 gets one outdoors from GPS. `status` shows `gps sentences` climbing within a
     second, and `fixes` once it has a fix.
   - Indoors, or on the V3, store one on each board, a few hundred metres apart:
     - A: `setpos 46.8700 -113.9900`
     - B: `setpos 46.8730 -113.9900`
2. **Check the radio.** Within about a minute, beacons go out. On each board, `status` should show `rx`
   counting up and `nb 1`.
3. **Swap keys.**
   - On A, run `keys` and copy the line. On B, run `peer <that line>`.
   - On B, run `keys` and copy the line. On A, run `peer <that line>`.
   - Each should print `ok`.
4. **Encrypted DM.**
   - On A, run `send <B-id> hello from A`.
   - B prints `MSG from <A-id> ...: hello from A`.
   - A prints `DELIVERED to <B-id> ...`.
5. **Check the encryption.**
   - On B, run `requireenc on` then `reboot`.
   - On a third board with no keys, or on A after `wipe confirm` (which makes a new identity), send to
     B. B should **not** print the message, and the `authfail` counter in B's `stats` goes up by one.
   - Then go back: on B, `requireenc off`, `reboot`, and redo step 3 for any wiped board.
6. **SOS.**
   - On A, run `sos twisted ankle, need help`.
   - B prints `SOS from <A-id> seq 1 at <lat> <lon>: twisted ankle, need help` and the matching
     `sosack` line.
   - On B, run `sosack <A-id>`. A prints `SOSACK from <B-id> for seq 1`.
   - On A, run `sosstop`. Running `sos` again within 60 s is rate limited, as intended.
7. **Mixed pair (optional).** Repeat steps 3–6 between a dev board and a Pi running `ridgelined`. The
   `keys` lines are interchangeable with `ridgelined` peer-file lines.

Record each board's `status` output at the end of the test. The `air`, `tx` and `rx` counters and the
heap numbers are what to compare across firmware versions.
