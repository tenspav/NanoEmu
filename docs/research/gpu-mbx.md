# S5L8740/S5L8747 PowerVR MBX 2D/3D GPU Subsystem Reference

The graphics subsystem incorporates an Imagination Technologies PowerVR MBX Lite core operating alongside dedicated 2D image scaling and display plane hardware. The 3D engine interfaces via memory-mapped command FIFOs, register-pair job arrays, and an on-chip translation table MMU, signaling completion through VIC IRQ 53.

## Subsystem Address Map

The graphics architecture spans separate MMIO blocks for 2D plane management, core 3D pipeline registers, and command FIFO apertures.

| Address Range | Size | Description |
|---|---|---|
| `0x39100000` - `0x39100fff` | 4 KB | 2D image and scaling engine |
| `0x39200000` - `0x39200fff` | 4 KB | 2D dual image plane controller |
| `0x39300000` - `0x39300fff` | 4 KB | Video output configuration |
| `0x3b000000` - `0x3b003fff` | 16 KB | PowerVR MBX 3D core registers and MMU |
| `0x3b800000` - `0x3b9fffff` | 2 MB | Command FIFO window 0 |
| `0x3ba00000` - `0x3bbfffff` | 2 MB | Command FIFO window 1 (compositor stream) |
| `0x3bc00000` - `0x3bdfffff` | 2 MB | Command FIFO window 2 |

## 2D Engines and Scaling Logic

### Image / Scaling Engine (`0x39100000`)
- Control: Programmed by `0x0825327c`.
- Offsets `+0x28`, `+0x2c`, `+0x30`, `+0x34`: Source coordinate and cropping ranges.
- Offset `+0x08`: Writing `1` triggers the scaling operation.

### Dual Image Planes (`0x39200000`)
- Plane 0: Format and control at `+0x20`, physical framebuffer base at `+0x24`, geometry bounds at `+0x28`, `+0x30`, `+0x34`.
- Plane 1: Format and control at `+0x40`, physical framebuffer base at `+0x44`, geometry bounds at `+0x48`, `+0x50`, `+0x54`.

### Video Output Configuration (`0x39300000`)
- Offset `+0x00`: Subsystem control (initialized by `0x08253004`).
- Offset `+0x08`: Output configuration word.
- Offset `+0x40`: Status register, checked with `status & 1`.

### Shutdown Sequence (`0x0825281c`)
1. Clear bit 0 at `0x39300000`, `0x39200000`, and `0x39100000`.
2. Poll bit 1 at each base address until set.

## PowerVR MBX Core Registers (`0x3b000000`)

Offsets below are relative to core base `0x3b000000`.

| Offset | Name | Description |
|---|---|---|
| `+0x80` | MBX_RESET_CTRL | Core reset/control: write `0x111`, then write `0` (`0x0812b600`) |
| `+0x12c` | MBX_INT_STATUS | Interrupt status and FIFO occupancy (detailed below) |
| `+0x130` | MBX_INT_ENABLE | Interrupt enable mask: initialized to `0x8775` by `0x0812b90c` |
| `+0x134` | MBX_INT_CLEAR | Interrupt write-1-to-clear acknowledgement (`0xffff` during init) |
| `+0x608` | MBX_PIPE_CFG0 | Render pipeline configuration register 0 (`0x08195e50`) |
| `+0x610` | MBX_PIPE_CFG1 | Render pipeline configuration register 1 |
| `+0x674` | MBX_PIPE_CFG2 | Render pipeline configuration register 2 |
| `+0x6ec` | MBX_JOB_ADDR | 3D scheduler job address register (written by `0x0813645a`) |
| `+0x808` | MBX_RENDER_RUN | Render trigger register: writing `1` initiates rendering |
| `+0x85c` | MBX_CORE_CFG | Core hardware configuration: set to `0x80` during init |
| `+0xf00` | MBX_ID_FALLBACK0 | Synthetic ID fallback word 0: `0x01020003` |
| `+0xf10` | MBX_ID_FALLBACK1 | Synthetic ID fallback word 1: `0x00010500` |
| `+0xff0` - `+0xffc` | MBX_CORE_ID | Core hardware identification: checked for `0xb105f00d` (`0x0813f574`) |

## Interrupt Handling and IRQ 53

- Interrupt Vector: VIC IRQ 53 (`0x35`), installed by `0x080df780`.
- VIC Slot: `0x08ae6894` points to dispatcher `0x0814e321`.
- Dispatcher: Walks the handler chain rooted at `0x089098a8`. Node `0x09984a3c` calls `0x08191de5` passing state argument `0x09984890`.
- Core Handler (`0x08191de4`): Disables interrupts, inspects `+0x12c`, executes callbacks, acknowledges active bits via `+0x134`, and restores enables.

### Interrupt Status Register Bits (`+0x12c`)
- Bit 4 (`0x10`): Render completion. Routine `0x08191e64` advances render-completion state.
- Bit 5 (`0x20`): Tiled job completion. Routine `0x0814fb10` advances tiled-job state.
- Bits 2, 3 (`0x0c`) and Bit 6 (`0x40`): Paired completion flags.
- Bits 8, 9 (`0x100`, `0x200`): Extended operation completion flags.
- Bit 10 (`0x400`): Callback notification invoked through `0x08198f1e`.
- Bits [23:16]: FIFO occupancy counter.

## GPU Memory Management Unit (MMU)

The MBX core translates virtual graphics addresses into physical RAM pages via eight hardware translation tables.

- Table Allocation: Object `0x099655f4` manages eight 4096-byte page tables located at `0x0995c000`, `0x0995d000`, `0x0995e000`, `0x0995f000`, `0x09960000`, `0x09961000`, `0x09962000`, and `0x09963000`.
- Base Configuration: `0x084850e4` writes physical table addresses to `0x3b001000` through `0x3b00101c` (`+0x1000` to `+0x101c`).
- Translation Control (`+0x1020`):
  - Bit 0: Translation enable toggle.
  - Bit 8: Invalidation enable.
  - Bit 16: Translation status, polled by `0x08484e94` and `0x08484df4`.
- Invalidation Registers: Invalidation parameters are written to `+0x1024` and `+0x1028` before toggling bit 8 of `+0x1020`.

## Command Submission Architecture

Command submission uses two mechanisms: streaming FIFO windows and RAM-based register-pair arrays.

### FIFO Apertures (`0x3b800000`, `0x3ba00000`, `0x3bc00000`)
- Aperture Size: Each aperture spans 2 MB (`0x200000` bytes).
- Streaming Writers: `0x081805dc` and `0x081806ac` write successive words to the fixed MMIO aperture address.
- Compositor Submission: `0x08198e5a` streams commands into `0x3ba00000`.
- Flush Barrier (`0x0847ffb8`): Sends seven synchronization words from `0x08752d34`: `0xf0000000` followed by six words of `0x70000000`.
- Capacity Calculation (`0x0817b508`): Available FIFO capacity is computed as:
  ```
  available_bytes = (0x7b - ((read_32(0x3b00012c) >> 16) & 0xff)) * 4
  ```

### Register-Pair Job Lists
- Scheduler: 3D job scheduler resides at `0x0813645a`.
- Job Structure:
  - `+0x20`: Number of register-value pairs.
  - `+0x24`: Start of array containing `(offset, value)` elements.
- Execution Worker: `0x083f1a88` traverses the array and writes each value to `0x3b000000 + offset`.
- Scheduler writes the completed job pointer to `0x3b0006ec`.

## System Tasks and Compositor Pipeline

- Core Engine String: `"MBX1"` at `0x08105e2c` (referenced at `0x08105e18`, `0x08105e20`). Timeout control symbol `_disable_mbx_timeout` at `0x08808650` (ref `0x08753e48`).
- Flush Thread (`MBXFlushThread`, `0x081dd104`): Entry `0x081dd021`, body `0x081dcba0`, thread object `0x099821fc`. Queue descriptor at `0x09982210` (capacity 16, record size 32 bytes at `0x09981fec`). Records contain data words at `+0x00..+0x14`, operation byte at `+0x18`, and completion semaphore at `+0x1c`.
- Flip Thread (`MBXFlipHandlerThread`, `0x08254f14`): Entry `0x08254ea3`, body `0x08254e34`, thread object `0x09984808`. Indices at `0x09984808/0c`. Flip callback ring `0x09984814` holds eight 12-byte records `{object, argument, sync}`. Enqueue function is `0x08254e0b`.
- Compositor Interface (`GKMBX2D`, `0x0847f88c`): Object pointer returned by `0x08402075` is `0x09989344` (vtable `0x087e7704`, worker entry `0x084806e3`, body `0x08480218`).
  - Vtable `+0x10` (`0x08480605`): Queue operation 0.
  - Vtable `+0x1c` (`0x08480681`): Queue operation 1 (drawing descriptor wrapper).
  - Vtable `+0x24` (`0x0847fff5`): Queue operation 6 (synchronization barrier).
  - Work Queue (`0x09989374`): Capacity 32, record size 272 bytes (`0x110`), signaled via semaphore `0x09989358`.
