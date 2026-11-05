# SX1262 radio-port integration acceptance contract

Status: design and verification criteria only. **No hardware is approved for flashing or transmitting.**

## Required integration behavior

- Keep the existing `gw::RadioPort` interface usable by host simulations; implement hardware behind a separate adapter.
- Initialization must fail closed if board identity, radio pins, SPI, BUSY, DIO1, RESET, antenna switching, regulator, or TCXO requirements are unknown.
- Never report a successful transmission without a completed radio operation. Surface timeouts and errors to the caller.
- Enforce packet length and bounds before copying buffers into the driver.
- Treat channel busy, SPI failure, TX timeout, and radio reset as distinct observable outcomes.
- After a radio fault, require an explicit recovery path; do not silently retry indefinitely.
- Keep transmission disabled by default in development and CI; no probe may transmit.

## Host-verifiable acceptance

1. A fake SX1262 driver can simulate BUSY stuck high, SPI failure, IRQ timeout, and successful TX without physical RF.
2. Tests prove invalid board configuration cannot initialize a transmitter.
3. Tests prove TX requests return failure when the driver fails or times out.
4. Existing virtual-mesh and firmware host tests continue to pass.
5. Hardware-specific adapter code is not compiled into the nontransmitting probe.

## Hardware-only acceptance (pending)

- Confirm manufacturer pinout, TCXO supply and startup timing, RF switch control, and regional settings for each exact board revision.
- Verify receive, transmit, channel activity, and fault recovery with suitable RF test equipment and compliant settings.
- Capture logs and hardware revision for every result; do not infer physical success from a host mock.

Until the hardware-only criteria are complete, the release gate remains blocked.
