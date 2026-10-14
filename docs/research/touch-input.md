# S5L8740/S5L8747 Touchscreen and Input Subsystem Reference

The touch input subsystem consists of a multi-touch controller attached to Synchronous Serial Port 2 (SSP2) at `0x3d200000`, backed by GPIO edge interrupts on bank 4, pin 6. Hardware buttons are handled through separate GPIO interrupt lines and an I2C power management slave.

## SSP2 Transport Interface

Touch panel communication uses SPI controller 2 (SSP2) mapped at MMIO base `0x3d200000`.

| Offset | Name | Description |
|---|---|---|
| `+0x00` | SSP2_CTRL | SPI bus and protocol control |
| `+0x04` | SSP2_TX_CTRL | Transfer and burst control |
| `+0x08` | SSP2_STATUS | Status flags and write-1-to-clear interrupt status |
| `+0x0c` | SSP2_CS_CTRL | Chip select control: bit 1 asserted during transfer |
| `+0x10` | SSP2_TX_DATA | Transmit data FIFO register |
| `+0x20` | SSP2_RX_DATA | Receive data FIFO register |
| `+0x34` | SSP2_RX_CNT | Number of bytes available in receive FIFO |
| `+0x4c` | SSP2_TX_CNT | Number of bytes remaining in transmit FIFO |

### Interrupts and Scheduling
- SSP2 Bus Interrupt: VIC IRQ 11 (vector entry `0x08000c94` -> handler `0x08000fec`). Transfer completion wakes event `0x44`.
- Touch Interrupt (INT line): GPIO bank 4, pin 6 (GPIO `0x26`). Level register `0x3cf00084` bit 6.
- GPIO Interrupt: VIC IRQ 32 (entry `0x08000912` -> `0x083a33be` -> callback `0x08163ab0`).
  - Pending register: `0x397000a4` bit 30.
  - Mask register: `0x397000c4` bit 30.
  - Configuration register: `0x39700084` bit 30.
  - Pending status is acknowledged by writing `1` back to bit 30 of `0x397000a4`.
- Driver Task: `MultitouchTask` (Thread 11, entry `0x080eec30`, IRQ callback `0x08163ab0`, wakes on event `0x43`). Transport routines reside at `0x08400430` -> `0x083f4ee8`.

## Packet Protocol

Transfers use static buffers: RX buffer at `0x08a97400`, TX buffer at `0x08a97600`, maximum transfer size 512 bytes.

### Length Query Transaction (`0x0817db98`)
1. Transmit Frame (16 bytes):
   - Bytes [0..2]: `0xEA 0x01 0x00`
   - Bytes [3..13]: Padding
   - Bytes [14..15]: Little-endian 16-bit sum of bytes 0 through 13 (`sum(bytes[0..13])`)
2. Response Frame:
   - Byte 1: Little-endian 16-bit payload length
   - Bytes [14..15]: Little-endian 16-bit checksum of response bytes 0 through 13

### Data Fetch Transaction (`0x08179b18`)
1. Transmit Frame (3 bytes): `0xEA 0x01 0x01`.
2. Response Frame:
   - Length: returned `length + 5` bytes.
   - Byte 0: `0xEA` magic byte.
   - Byte 1: Response type.
   - Bytes [2..3]: Little-endian 16-bit total payload length including trailing checksum.
   - Byte 4: Header checksum, validating `sum(bytes[0..4]) & 0xff == 0`.
   - Bytes [5..N]: Payload body.
   - Trailing 2 bytes: Little-endian 16-bit sum of all payload body bytes preceding the checksum.

### Contact Record Parsing (`0x081832cc`)
Payload structure:
- `payload[0]`: Magic identifier `0x44`.
- `payload[2]`: Offset to first contact record.
- `payload[0x10]`: Number of reported contacts.
- `payload[0x11]`: Stride between contact records.

Each contact record contains the following fields:

| Offset | Type | Description |
|---|---|---|
| `+0` | uint8 | Contact tracking ID |
| `+1` | uint8 | Contact state: `3` = new touch, `4` = move, `>= 5` = release |
| `+2` | uint8 | Contact type: values `1` through `5` are accepted |
| `+4` | int16 | Raw X coordinate (little-endian signed word) |
| `+6` | int16 | Raw Y coordinate (little-endian signed word) |

Contact slots are tracked at `0x08a973d0 + 6*n` (`+0` active byte, `+1` ID byte; 8 slots supported).

### Coordinate Mapping
The screen resolution is 240x432 pixels (display descriptor `0x08741a7c` specifies width 240 at `+0x0c`, height 432 at `+0x10`). Raw digitizer coordinates map to screen coordinates via:

```
screen_x = trunc((raw_x + 75) * 239 / 2845)
screen_y = 431 - trunc((raw_y + 75) * 431 / 5250)
```

## Input Queues and Event Delivery

Parsed touch events are dispatched through `0x08143ba4` -> `0x084046a8`.

- Per-Contact Queues: Located at `0x089a5298 + 24*n`. Staging coordinate buffers reside at `0x089a5210 + 8*n`.
- Queued Record Format:
  - `+0x00`: Contact ID (uint32)
  - `+0x04`: Event kind (uint8: `0` = down, `1` = up, `2` = move)
  - `+0x08`: Screen X coordinate (int32)
  - `+0x0c`: Screen Y coordinate (int32)
- Notification Pipeline: `0x08173eb4` -> `0x08418860` -> `0x0801a344`.
- Event Ring Buffer: Head pointer at `0x089090d8` (points to `0x0b432d20`); insertion ring at `0x0b432d2c`.
- Input Enable: System input delivery requires setting bit 0 of `0x08909068`.

## Hardware Buttons

### Volume Buttons
- Volume Up: GPIO `0x28` (bank 4, pin 8), level at `0x3cf000a4` bit 0 (active-low).
- Volume Down: GPIO `0x29` (bank 4, pin 9), level at `0x3cf000a4` bit 1 (active-low).
- Interrupt: VIC IRQ 31.
  - Pending register: `0x397000a8` bits 0 and 1.
  - Mask register: `0x397000c8` bits 0 and 1.
  - Callback: `0x081541f4`.
- Driver Task: `GPIOButtonManagerTask` (Thread 20, entry `0x080de2cc`). Debounce time is 10,000 microseconds; emits button events through `0x08408570`.

### Home and Power Buttons
- Bus: I2C Controller 1 at base `0x3c900000`.
- Device Address: 7-bit slave address `0x73`.
- Register: Address `0x07`.
  - Bit 4: Power button (active-low).
  - Bit 5: Home button (active-low).
- Sampling and Interrupt:
  - Wakeup line: GPIO `0x56` asserts on button state change.
  - Polling routine: `0x083a6f80`.
  - Dispatch routine: `0x080fb270`.
