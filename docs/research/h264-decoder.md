# S5L8740/S5L8747 Apple M2 H.264 Video Decoder Reference

The video decoding subsystem incorporates an Apple M2 multi-pipe H.264 decoder mapped across MMIO addresses `0x39600000` through `0x39670000`. Frame decoding operations are scheduled via hardware doorbell registers and signal completion through VIC IRQ 45.

## Sub-Block Architecture

The hardware decoder partitions tasks across specialized processing sub-blocks within the peripheral memory window.

| MMIO Aperture | Sub-Block | Functional Role |
|---|---|---|
| `0x39600000` | Top Controller | Power gating, global resets, and top-level IRQ routing |
| `0x39610000` | Sub-Block 1 | NAL parser, entropy decoding, and bitstream parameters |
| `0x39630000` | Sub-Block 3 | Pipeline sequencer, status polling, and doorbell trigger |
| `0x39640000` | Sub-Block 4 | Inverse quantization scaling lists |
| `0x39650000` | Sub-Block 5 | Decoder core profile and slice header parameters |
| `0x39660000` | Sub-Block 6 | Motion compensation, reference frames, and frame output |

## Register Map

### Top Controller (`0x39600000`)
- `+0x00`: Top IRQ status and write-1-to-clear register:
  - Bit 1: Sub-block 63 interrupt pending
  - Bit 6 (`0x40`): Sub-block 66 frame completion interrupt pending
  - Bit 8: Sub-block 61 interrupt pending
  - Handled by ISR `0x080ec8d8`
- `+0x04`: Top mode selection (`0x142`)
- `+0x0c`: Power and clock gating: write `0` to assert reset, write `1` to enable
- `+0x10`: Control register (`0x80000000`)
- `+0x1c`: Initialization word (`0xffffffff`)
- `+0x2c`: Initialization word (`2`)
- `+0x1004`: Core software reset register: polled until bit 0 clears to `0`
- `+0x10d8`: Reset bit control register

### Sub-Block 1: Bitstream Engine (`0x39610000`)
- `+0x00`: Interrupt status and write-1-to-clear (`0x0812ebbc`)
- `+0x04`: Configuration register (`0x3f`)
- `+0x0c`: Execution run register (`12`)
- `+0x10`, `+0x14`, `+0x1c`, `+0x34`, `+0x9c`, `+0xa0`: NAL bitstream parameters

### Sub-Block 3: Pipeline Sequencer and Doorbell (`0x39630000`)
- `+0x04`, `+0x20`: Status polling registers: polled until zero
- `+0x100`: Execution IRQ status / W1C / arm register (`0x20`)
- `+0x104`: Execution control register (`20`)
- `+0x110`: Hardware doorbell trigger: write `2` or `0x102` (`0x0812ebee`) to start execution

### Sub-Block 4: Inverse Quantization (`0x39640000`)
- `+0x1200`, `+0x1300`: Quantization scaling matrices (64 words each, programmed by `0x0814e76e` - `0x0814e778`)
- `+0x1804`: Transform configuration word (`0x0812eaf0`)

### Sub-Block 5: Profile Configuration (`0x39650000`)
- `+0x00`: Status and write-1-to-clear register
- `+0x10`: Core profile register: programmed with `0xc02 | flags` (`0x0812eafe`)

### Sub-Block 6: Motion Compensation and Output (`0x39660000`)
- `+0x00`: Frame IRQ status and write-1-to-clear: bit 1 indicates decode completion (`0x080ec8f6`)
- `+0x04`: Sub-block initialization word (`0x12`)
- `+0x0c`: Start decode command: write `3` (`0x0812ebf6`) to start decode engine
- `+0x10`: Frame mode selection (`0x200`)
- `+0x18`: Input bitstream DMA address / Destination Y luma plane address (`0x0814e6f6`, `0x0812ebdc`)
- `+0x1c`: Input bitstream size in bytes
- `+0x20`: Destination Cb chroma plane physical address (`0x0814e6fe`)
- `+0x24`: Destination Cr chroma plane physical address
- `+0x2c`, `+0x34`: Reference frame 0 Y and CbCr addresses (bit 31 masked)
- `+0x3c`, `+0x44`: Reference frame 1 Y and CbCr addresses (bit 31 masked)
- `+0x6c`: Frame geometry descriptor (`0x0815a648`)
- `+0x78`, `+0x7c`, `+0x80`: Auxiliary motion vector buffers

## Decode Submission and Interrupt Dispatch

### Submission Flow
1. Load input bitstream buffer address into `0x39660018` and byte length into `0x3966001c`.
2. Configure destination planar framebuffer pointers: Y at `0x39660018`, Cb at `0x39660020`, Cr at `0x39660024`.
3. Configure reference frame pointers at `0x3966002c` and `0x3966003c`.
4. Trigger hardware doorbell by writing `2` or `0x102` to `0x39630110` (`0x0812ebee`).
5. Assert frame decode trigger by writing `3` to `0x3966000c` (`0x0812ebf6`).

### Interrupt Signaling
1. Hardware asserts bit 1 of `0x39660000` (sub-block completion) and bit 6 of `0x39600000` (top-level completion).
2. The core asserts VIC IRQ 45 (Bank 1, bit 13; interrupt controller glue register `0x38e0200c` bit 13 / `0x2000`).
3. The VIC routes IRQ 45 through vector slot `0x08ae6874` (`0x08ae67b8 + 8 + 4*45`) to ISR `0x080ec8cd`.
4. Interrupt handler `0x080ec8cc` acknowledges the status bits on `0x39660000` and `0x39600000`, then signals the completion semaphore at `0x08811d50` through helper `0x08427a66`.

### Driver Architecture
- Driver Class: `AppleM2H264MPDDriver` (vtable `0x0879e9d0`, referenced at `0x08065e6c`, `0x081d7748`, `0x083ddffc`, and `0x083e3558`).
- Picture Buffer Descriptor: String reference at `0x0871b570` (`0x083e34d4`).
- Core Driver Functions:
  - Global Reset: `H264_MPD::SwResetEntirely()` at `0x08495870`.
  - Core Reset: `H264_MPD::SwResetForCore()` at `0x08494fa0`.
  - Parsing Pipeline: `H264_MPD::ParsingStage()` at `0x08493d3c`.
  - Completion Handler: `frame_decode_complete()` at `0x08496be4`.
  - Frame Dispatch: `H264_DecoderDriver::decodeFrame()` at `0x08232ca4` - `0x08232cb4`.

## Minimal Emulation Requirements

An emulator stub can satisfy driver initialization and frame processing with the following responses:
- Reset Polling: Reading `0x39601004` returns `0` (reset complete). Reads to `0x39630004` and `0x39630020` return `0`.
- Top Status (`0x39600000`): Returns `0x40` when an interrupt is pending. Writing back `0x40` clears the pending flag and lowers IRQ 45.
- Sub-Block Status (`0x39660000`): Returns `0x02` when frame decoding finishes. Writing back `0x02` clears the flag.
- Trigger Handling: Writing to doorbell `0x39630110` or start register `0x3966000c` sets status bit `0x02` on `0x39660000` and bit `0x40` on `0x39600000`, asserting IRQ 45.
