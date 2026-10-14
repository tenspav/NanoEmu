# S5L8740/S5L8747 LCD Controller Specification

The display subsystem consists of an LCD timing and DMA controller at MMIO base `0x38900000`, panel transport logic at `0x38300000` and `0x3d800000`, and an SPI backlight interface at `0x3de00000`. The physical screen resolution is 240x432 with a 32-bit BGRA8888 framebuffer format.

## Panel Geometry and Display Pipeline

Screen dimensions are defined in hardware descriptors at `[0x08741a88] = 240` (width) and `[0x08741a8c] = 432` (height). Timing register `+0x1b24` encodes resolution as `((width - 1) << 16) | (height - 1)`, resulting in `0x01af00ef`.

```
+------------------------------------------------------------------------+
| GKCompositor_CLCD_G (Thread 64) -> Display Object (0x0998a568)         |
|  - Surface Ring Buffer: 414,720 bytes BGRA8888 (Stride 960 bytes)      |
+------------------------------------------------------------------------+
                               |
                               v
+------------------------------------------------------------------------+
| LCD Controller (0x38900000)                                            |
|  - Primary Layer Base (+0x24): 0x0bf29380 / 0x0bec3f00                 |
|  - Layer Format (+0x20): Selector 7 (4 bytes/pixel, BGRA8888)          |
|  - Geometry (+0x1b24): 240x432 active area                             |
+------------------------------------------------------------------------+
                               |
                               v
+------------------------------------------------------------------------+
| Panel Transport (0x38300000, 0x3d800000)                               |
|  - Window commands 0x2a, 0x2b, pixel push 0x2c                         |
|  - Transfer trigger 0x38300080, busy check 0x3830008c                  |
+------------------------------------------------------------------------+
                               |
                               v
+------------------------------------------------------------------------+
| Physical LCD Panel (240x432)                                           |
|  - Tearing Effect (TE) synchronization via GPIO Pin 0x0e (IRQ 32)      |
|  - Backlight control via SPI at 0x3de00000 (IRQ 52)                    |
+------------------------------------------------------------------------+
```

## Register Map (`0x38900000`)

Offsets below are relative to controller base `0x38900000`. Read-modify-write operations preserve unmentioned bits.

| Offset | Name | Description |
|---|---|---|
| `+0x00` | LCD_CTRL | Controller control: bit 0 enable, bit 7 (`0x80`) master enable |
| `+0x04` | LCD_LAYER_EN | Layer enable mask: bit 4 (`0x10`) primary, bit 5 (`0x20`) layer 1, bit 8 (`0x100`) layer 2 |
| `+0x08` | LCD_INT_CTRL | Interrupt control/status (`0x0805652c` writes `0x700`; installs IRQ 13 panic handler) |
| `+0x20` | LCD_L0_FMT | Layer 0 format control: bits [11:8] selector 7 (4 bytes/pixel) |
| `+0x24` | LCD_L0_BASE | Layer 0 physical framebuffer base address (`surface->getBuffer() & 0x7fffffff`) |
| `+0x3c` | LCD_TIMING0 | Timing register 0 (set to `(old & ~0xfff) | 0x60`) |
| `+0x44` | LCD_L1_BASE | Layer 1 physical framebuffer base address |
| `+0x5c` | LCD_TIMING1 | Timing register 1 (set to `(old & ~0xfff) | 0x60`) |
| `+0x100` | LCD_STATUS | Controller status / hardware revision (read into `display_object+0x7d0`) |
| `+0x114` | LCD_WIN_BOUND | Window bounds: fields [27:16] and [11:0] set to `0x10` |
| `+0x118` | LCD_WIN_CTRL | Window control (set to `(old & ~4) | 3`) |
| `+0x11c` | LCD_L2_XSTART | Layer 2 window horizontal start position |
| `+0x120` | LCD_L2_YSTART | Layer 2 window vertical start position |
| `+0x124` | LCD_L2_XEND | Layer 2 window horizontal end position |
| `+0x128` | LCD_L2_YEND | Layer 2 window vertical end position |
| `+0x300` | LCD_COLOR_MODE | Dither and color mode control (`(old & ~0x33330001) | 0x81110001`) |
| `+0x304` | LCD_COLOR_DEPTH | Color depth configuration (`0x080808`) |
| `+0x408` | LCD_LUT_SEL | LUT table selector prefix register |
| `+0x40c` | LCD_LUT_DATA | LUT table 32-bit data register |
| `+0x1b10` | LCD_GEN_CTRL | Timing generator control: bit 0 run, bit 16 (`0x10000`) active enable |
| `+0x1b14` | LCD_TIMING_A | Timing generator parameter A (`2`) |
| `+0x1b18` | LCD_TIMING_B | Timing generator parameter B (`0`) |
| `+0x1b1c` | LCD_SYNC_TIMING | Sync timing configuration (`0x00010109`) |
| `+0x1b20` | LCD_PORCH_TIMING| Back porch timing configuration (`0x0001011b`) |
| `+0x1b24` | LCD_RES | Active display resolution: `0x01af00ef` (`((width-1)<<16) | (height-1)`) |

## Initialization Sequence

The display driver entry point is `0x08044350`. Cold initialization calls `0x0804c584` (`r1 = 0`); resume calls `0x08247ca0` (`r1 = 1`).

1. Timing Presets: `0x08044350` programs `+0x3c` and `+0x5c` to `(old & ~0xfff) | 0x60`.
2. Controller Quiesce (`0x0804c584`):
   - `+0x1b10 &= ~1`
   - `+0x00 &= ~1`
   - `+0x04 |= 0x100`
3. Color Lookup Table (LUT) Programming (`0x08052468`):
   - Prefix calculation: `prefix = (read(+0x408) + 0x408) & ~0x1f0ff`.
   - Table 1 (225 entries, `i = 0..224`): write `+0x408 = prefix | 0x17000 | i`, write `+0x40c = [0x0890a810 + 4*i]`.
   - Table 2 (256 entries, `i = 0..255`): selectors `0x11000 | i`, `0x12000 | i`, `0x13000 | i` loaded with `[0x0890ab94 + 4*i]`.
   - Table 3 (256 entries, `i = 0..255`): selectors `0x14000 | i`, `0x15000 | i`, `0x16000 | i` loaded with `[0x08909c10 + 4*i]`, `[0x0890a010 + 4*i]`, `[0x0890a410 + 4*i]`.
4. Dither and Depth Configuration:
   - `+0x300 = (old & ~0x33330001) | 0x81110001`
   - `+0x304 = 0x080808`
5. Display Engine Enable:
   - `+0x00 |= 0x80`
   - `+0x04 &= ~2`
   - `+0x04 &= ~0x100`
6. Timing Generator Setup:
   - `+0x1b14 = 2`
   - `+0x1b1c = 0x00010109`
   - `+0x1b20 = 0x0001011b`
   - `+0x1b24 = 0x01af00ef`
   - `+0x1b18 = 0`
7. IRQ Setup (`0x0805652c`): Programs `+0x08 = 0x700` and registers VIC IRQ 13.
8. Cold Setup Finalization (`0x08044350`):
   - `+0x114` fields [27:16] and [11:0] set to `0x10`
   - `+0x118 = (old & ~4) | 3`
   - Reads `+0x100` into `display_object+0x7d0`
9. Activation:
   - `+0x00 |= 1`
   - Glue register `0x38300000 |= 0x400`
   - Glue register `0x38300070 = 1`
   - `+0x1b10 |= 0x10000`

## Framebuffer Memory Architecture

The active display object resides at `0x0998a568` (vtable `0x087a991c`).

- Buffer Ring: Current ring slot index is stored at `[object+0xc0]` (`6`). The descriptor array is at `[object+0xc8 + 4*n]`.
- Active Surface: Surface descriptor pointer is `0x098f33d0`. Getter function `0x084aa40a` reads `surface+0x34` (`0x8bf29380`), providing physical address `0x0bf29380`. Alternate active buffer is at `0x0bec3f00`.
- Format and Resolution: 240x432 pixels, 32 bits per pixel BGRA8888 (byte order: B, G, R, A; 32-bit LE word `0xAARRGGBB`).
- Memory Layout: Stride is `0x3c0` (960 bytes = 240 * 4). Full frame allocation size is `0x65400` (414,720 bytes = 240 * 432 * 4).
- Base Address Programming: `0x08080b70` programs `+0x24` with the masked physical buffer address.

## Refresh Control, Panel Transport, and Backlight

### Frame Refresh and Tearing Effect (TE) Synchronization
- Frame Push Request: `0x08405e26` sets `[display_object+0x770] = 1`.
- TE Synchronization: Connected to GPIO pin `0x0e` (group 1, bit 6).
- VIC IRQ 32: Handler `0x08000912` -> `0x083a33be`. GPIO status register `0x39700084` bit 6 (`0x40`), enable register `0x397000a4` bit 6 (`0x40`). The interrupt is cleared by writing `0x40` to `0x39700084`.
- TE Callback Chain: `0x08060b18` -> `0x0807558c`, which triggers layer updates via `0x08080b70`.
- Display Threads:
  - Thread 59: `"CLCD Buffer release"` (entry `0x080564e0` -> `0x08066a10`, waits on event `0xeb`).
  - Thread 64: `"GKCompositor_CLCD_G"` (thread descriptor `0x08ab0c88`).

### Panel Transport Interface
- Register `0x38300080`: Frame transfer control (`0x080a06c8` writes 1, transmits panel window commands `0x2a`, `0x2b`, `0x2c`, and writes 0).
- Register `0x3830008c`: Busy status; `0x080a74ac` polls until bits [1:0] clear.
- Transport Core (`0x3d800000`): Panel command packet headers are written to `+0x34`; command payload bytes are written to FIFO `+0x38` via `0x080749a4`.

### Backlight SPI Controller
- Driver Call Chain: `0x081cbcc2` -> `0x0854fa30` -> `0x084f6688` -> `0x08167cd8` -> `0x081304d4`.
- SPI Peripheral Base: `0x3de00000`.
- Control Sequence: Write `2` to `0x3de00014`, write 16-bit command word to `0x3de00018`, write `0x3de00014 |= 1` to initiate SPI transmission.
- Completion: Generates VIC IRQ 52 and signals event `0x3c`.
