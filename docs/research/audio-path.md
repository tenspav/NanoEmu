# S5L8740/S5L8747 Audio Subsystem Reference

The audio architecture comprises three multichannel I2S controllers, a dual-engine DMA controller, and an external hardware audio codec connected over SPI. Audio playback streams through I2S Controller 0 using paced DMA transfers to the transmit FIFO.

## I2S Controller Interface

The SoC incorporates three identical I2S controller blocks selected via routine `0x08427b08`.

| Selector | MMIO Base | Function |
|---|---|---|
| 0 | `0x3ca00000` | Primary audio output (headphone/speaker path) |
| 1 | `0x3cd00000` | Secondary digital audio / dock line-out |
| 2 | `0x3d400000` | Auxiliary audio interface |

### Register Map (Offsets Relative to I2S Base)

| Offset | Name | Description |
|---|---|---|
| `+0x00` | I2S_CTRL | Controller reset/control: write 1 to reset (`0x080bdf38`); shutdown waits for bit 1 |
| `+0x04` | I2S_FORMAT | Audio serial format configuration (`0x080ba31c`) |
| `+0x08` | I2S_TX_CTRL | Transmit enable: start ORs with 6 (`0x080b3ee8`); stop clears bit 4 (`0x0854faf4`) |
| `+0x10` | I2S_TX_FIFO | Transmit audio sample FIFO (32-bit audio sample port, submission at `0x080b346e`) |
| `+0x34` | I2S_RX_CTRL | Receive enable and control |
| `+0x38` | I2S_RX_FIFO | Receive audio sample FIFO |
| `+0x3c` | I2S_STATUS | Status and write-1-to-clear register: bit `15 + direction` tested by `0x080bdec8` |
| `+0x40` | I2S_DIVISOR | Clock divider: 44,100 Hz sets `0x110`; 48,000 Hz sets `0xfa` (`0x080d077c`) |

I2S controllers do not generate independent interrupt requests. Buffer timing is driven by the system DMA controller.

## Audio Codec Control (SPI Controller 0)

Codec control uses an SPI bus rather than I2C. The firmware identifies the codec as device ID `0x4281` (dispatch table `0x0890c3d0`, driver descriptor pointer `0x0890bc68`).

### Host SPI Controller (`0x3c300000`)
- Peripheral: SPI controller 0.
- Chip Select: Regulated via `+0x0c` bit 1 (`0x083f50ec`).
- Registers: Status/W1C at `+0x08`, transmit FIFO at `+0x10`, receive FIFO at `+0x20`.
- Interrupt: VIC IRQ 9 (ISR `0x08000fec`). Transaction completion signals event `0x44`.

### Codec Command Protocol
- Single Register Write (`0x08429fb8`): Transmits 5 bytes:
  ```
  [0x6c] [reg_msb] [reg_lsb] [0x00] [data]
  ```
- Single Register Read (`0x08429ffe`): Transmits 5 bytes:
  ```
  [0x6d] [reg_msb] [reg_lsb] [0x00] [dummy]
  ```
- Stereo Register Write (`0x083eab80`): Transmits 6 bytes:
  ```
  [0x6c] [reg_msb] [reg_lsb | 0x80] [0x01] [left_data] [right_data]
  ```

### Codec Register Map and Sequences
- Unlock Sequence (`0x080d01b8`): Writes `0xa5` followed by `0x00` to register `0x9901`.
- Power and Mute Controls: Registers `0x0006`, `0x0007`, `0x000e`, and `0x0220`.
- Sample Rate Control: Registers `0x000f`, `0x012f`, `0x0130`, `0x0131`, and `0x0222`. Rate code for 44.1 kHz is `0x0a`.
- Signal Routing (`0x080d0220`): Programs registers `0x0203` through `0x0207`.
- Playback State Setup: Programs DSP registers `0xc81f = 0xff`, `0xc85f = 0x0f`, `0xc96f = 0x0e`, `0x0223 = 0x08`, and `0x0224 = 0x09`.
- Volume Control (`0x083b6a4c`): Output selector 0 uses registers `0x0227` and `0x0228`; output selector 1 uses `0x0229` and `0x022a`.
- Shutdown Polling (`0x080d073e`): Polls status register `0x002f` until bit 7 (`0x002f & 0x80`) clears.

## Direct Memory Access (DMA)

Audio buffer transmission uses multichannel DMA engines configured via allocator `0x080a309c`.

| Engine ID | MMIO Base | VIC IRQ | Channels |
|---|---|---|---|
| 1 | `0x38200000` | IRQ 17 | 8 |
| 2 | `0x38700000` | IRQ 16 | 8 |

### Channel Registers (`base + 0x100 + 0x20 * n`)
Peripheral request index 12 routes transfers to the primary I2S TX FIFO (`0x3ca00010`).

| Offset | Name | Description |
|---|---|---|
| `+0x00` | DMA_SRC | Source buffer physical memory address |
| `+0x04` | DMA_DST | Destination register address (`0x3ca00010`) |
| `+0x08` | DMA_NEXT | Pointer to next linked scatter-gather descriptor |
| `+0x0c` | DMA_CFG | Channel burst and priority configuration |
| `+0x10` | DMA_CTRL | Control word: submission `0x080b1cb4` sets `0x8001` |
| `+0x14` | DMA_COUNT | Transfer byte count |

### Descriptors and Interrupts
- Scatter-Gather Layout: Five 32-bit words `{SRC, DST, next, config, count}`.
- Interrupt Status: Engine register `+0x04` bit `n` indicates completion. Written back to `+0x08` bit `n` to acknowledge.
- Interrupt Service Routine: Common handler at `0x220010ee` dispatches channel completion events.

## Audio Tasks and Pipeline

- Task Object: `MeCCAOutputTask` (entry `0x080a6ec7`, created by constructor `0x080a2060` via `0x083e3bda`).
- Output Worker: `0x080b341c`, which invokes mixer callback `0x0809c5b8`.
- Transfer Granularity: The output thread requests 1024 bytes (`0x1000` bytes / `0x400` sample units) per iteration.
- Backend Resolution: Lazy getter `0x083e2090` selected by `0x08414d18`.
