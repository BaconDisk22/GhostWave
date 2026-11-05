# Radio recovery verification

- Simulate SX1262 initialization failure; report error and keep TX disabled.
- Simulate BUSY pin stuck high; enforce bounded timeout, never infinite loop.
- Simulate CRC error burst; restore RX without resetting identity.
- Simulate radio reset mid-transmission; reject partial packet and recover RX.
- Verify retry limits and watchdog behavior with mock SPI/GPIO.
- Repeat on T1000-E and RAK4631 hardware before release.
