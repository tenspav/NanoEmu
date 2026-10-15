# iPod nano 7G SoC Real H.264 Hardware Video Decoder Specification & Emulation Plan
# Target Block: MMIO 0x38f00000, VIC IRQ 35
# Driver: AppleM2H264MPDDriver / H264_MPD / H264_DecoderDriver

================================================================================
1. ARCHITECTURAL IDENTITY & HARDWARE VERIFICATION
================================================================================
Previous drafts conflated two distinct hardware video engines on the S5L8747 SoC:
  - 0x39600000 - 0x39670000, IRQ 45: Legacy MPEG-4 / H.263 engine (doorbell 0x39630110,
    start 0x3966000c, top IRQ 0x39600000 bit 6, child 0x39660000 bit 1, semaphore 0x08811d50).
  - 0x38f00000 - 0x38f03fff, IRQ 35: Real H.264 Multi-Pipe Decoder (MPD) macroblock & VLD
    engine. Base loaded at 0x08232d8e from literal 0x08232f24 (0x38f00000) into global
    context [0x088117e0 + 0x1c] (0x088117fc). IRQ 35 (0x23) registered at 0x08232d34 with
    handler 0x080df2d9; completion signaled via semaphore 0x08811d44.

Key Driver Code Locations (dumps/snap6/ram-full.bin, base 0x08000000):
  - 0x08232cfc: H264_DecoderDriver::init() (power on, IRQ install, work buffers, base init)
  - 0x082328f8: H264_DecoderDriver::decodeFrame() (sample loop, NAL parsing, slice dispatch)
  - 0x08493b10: H264_MPD::ParsingStage() (NAL DMA bounds, parser trigger, status check)
  - 0x08494890: H264_MPD::DecodingStage() (scaling lists, slice setup, trigger 0x38f01000 = 1)
  - 0x08495834: H264_MPD::SwResetEntirely() (reset request 0x38f01004 |= 0x7ff, poll bit 0)
  - 0x08494fa0: H264_MPD::SwResetForCore() (macroblock core reset)
  - 0x08496b18: H264_MPD::save_register_table() (reads 0x38f00000 MMIO into RAM backup)
  - 0x08496fa8: H264_MPD::restore_register_table() (restores 0x38f00000 MMIO from RAM backup)
  - 0x084964dc: H264_MPD::set_surface_slot() (programs Y/UV/aux shifted addresses i=0..19)
  - 0x080df2c0 / 0x080df2d8: IRQ 35 ISR (ack 0x38f010c0=0, test/clear 0x38f010b8.b4, post sem)
  - 0x08495b30 / 0x08495b64: post-decode check (read/W1C 0x38f01074, write 0x38f01508=0)

================================================================================
2. H.264 VLD / MACROBLOCK ENGINE REGISTER MAP (MMIO Base: 0x38f00000)
================================================================================
Aperture Size: At least 0x4000 bytes (0x38f00000 - 0x38f03fff). Access width is 32-bit.

| Offset Range       | Name / Function         | Access | Driver Semantics / Programming Sequence |
|--------------------|-------------------------|--------|-----------------------------------------|
| 0x0000 - 0x033f    | Core Config Bank 1      | R/W    | 0x340 bytes (832 B / 208 words). Saved at init by 0x08496b18, restored on reset by 0x08496fa8. |
| 0x0400 - 0x0cff    | Transform / Quant Bank  | R/W    | 0x900 bytes (2304 B / 576 words). Scaling matrices and transform parameters. Saved/restored across reset. |
| 0x1000             | Frame Decode Trigger    | W      | Write 1 to start macroblock decode engine (0x08494a3c). Preceded by clearing 0x1078 to 0. |
| 0x1004             | Software Reset & Status | R/W    | Full reset: write val \| 0x7ff, poll until bit 0 == 0 (0x08495834). Parser reset: (old & ~0xf) \| 0xc, poll bit 0 == 0. Max poll loop: 10,000 iters. |
| 0x1048             | Core Mode / Control     | R/W    | Preserved across reset from driver struct +0xe40. |
| 0x1074             | Decode Result / Status  | R/W1C  | Read decode result: 0 or 1 = success (0x08495b30). Bits 1..7 report errors: bit 1: VLC error, bit 2: MB error, bit 3: sub-motion, bit 4: pattern, bit 5: IDF, bit 6: delta-Q, bit 7: timeout. Written back with read value to acknowledge. |
| 0x1078             | Exp-Golomb / Engine Cmd | R/W    | Write 1: request unsigned Exp-Golomb parse (0x083c3560). Write 3: request signed Exp-Golomb parse (0x083c287c). Write 0: clear before starting frame decode. |
| 0x107c             | Exp-Golomb Result       | R      | Bit 7: ready (wait until 1). Bit 6: long result flag. Short unsigned: bits [20:8]. Short signed: arithmetic (word >> 8). Long result: prefix bits in [5:0], remainder read from syntax bank. |
| 0x10b8             | Malformed Error Status  | R/W    | Bit 4 indicates malformed bitstream. Tested and cleared by ISR 0x084974e0 via RMW (bic 0x10). Saved/restored from +0xe58. |
| 0x10c0             | IRQ Acknowledge         | W      | Write 0 acknowledges and deasserts IRQ 35 (0x0849716c). Zero-write, NOT write-1-to-clear. |
| 0x10c4             | Interrupt Config        | R/W    | Preserved across reset from driver struct +0xe48. |
| 0x10c8             | Interrupt Mask / Enable | R/W    | Programmed with 0xffff to enable IRQs (0x08497028). |
| 0x10d0             | Engine State Config     | R/W    | Preserved across reset from driver struct +0xe50. |
| 0x10d8             | Clock Gate / Reset Ctrl | R/W    | Gating control: RMW with `bic 3`, `orr 1` (0x08494b40). |
| 0x10e0             | Pipeline Config         | R/W    | Preserved across reset from driver struct +0xe44. |
| 0x1200             | Input Bitstream Base    | R/W    | Bits [21:0] = (input_phys >> 10). Aligned to 1024-byte boundary. |
| 0x1204 - 0x1208    | Work Buffer Pointers    | R/W    | Internal hardware scratch buffer physical page addresses. |
| 0x120c + 4*i        | Surface Slot Y Base     | R/W    | Slots i=0..19 (0x120c - 0x1258). Physical address of Luma (Y) plane shifted right 10: (Y_phys >> 10). |
| 0x125c + 4*i        | Surface Slot UV Base    | R/W    | Slots i=0..19 (0x125c - 0x12a8). Physical address of Chroma (UV) plane shifted right 10: (chroma_phys >> 10). |
| 0x12ac + 4*i        | Surface Slot Aux Base   | R/W    | Slots i=0..19 (0x12ac - 0x12f8). Physical address of Aux / MV buffer shifted right 10: (aux_phys >> 10). |
| 0x12fc + 4*i        | Dual-Surface Y2 Base    | R/W    | Slots i=0..19 (0x12fc - 0x1348). Optional secondary surface Y address (Y2 >> 10). |
| 0x134c + 4*i        | Dual-Surface UV2 Base   | R/W    | Slots i=0..19 (0x134c - 0x1398). Optional secondary surface UV address (UV2 >> 10). |
| 0x1400 + 4*(n&31)   | Syntax Bitstream Bank   | R      | Consumes fixed-width bit fields directly from NAL RBSP window (0x083c3588). |
| 0x1404             | Syntax Bit Read Reg     | R      | Successive reads consume single bits from the input bitstream (0x08496760). |
| 0x1480             | Syntax Lookahead Reg    | R      | Lookahead window into current RBSP position without advancing bit cursor. |
| 0x1508             | PPS Ext / Slice Status  | R/W    | Bit 13 checked during PPS parsing. Driver clears register to 0 after frame decode completion. |
| 0x1600             | Parser Trigger Register | R/W    | Programmed with (val & ~0xfff) \| 0x801 to initiate NAL header DMA parsing (0x08493bd6). |
| 0x1628             | Parser Status Register  | R/W    | Driver clears low 9 bits before parsing. Polled until Bit 8 == 1 (Ready). Bits [4:0] = NAL type (1=slice, 5=IDR, 7=SPS, 8=PPS). Bits [6:5] = nal_ref_idc (IDR requires > 0, SEI/AUD requires 0). |
| 0x1804             | Parser DMA Enable       | R/W    | Bit 0: set to 1 (`|= 1`) to enable parser DMA bitstream input (0x08493bbe). |
| 0x1808             | Parser Configuration    | R/W    | Preserved across reset from driver struct +0xe54. |
| 0x180c             | Input Window Start      | R/W    | Low 10 bits of bitstream start address: `input_phys & 0x3ff` (0x08493b70). |
| 0x1810             | Input Window End        | R/W    | End byte offset: `(input_phys & 0x3ff) + NAL_length` (0x08493b82). |
| 0x1814             | Parser Window Mode 1    | R/W    | Configured as `(val & 0xc0000000) \| 0x20` (0x08493ba2). |
| 0x1818             | Parser Window Mode 2    | R/W    | Configured as `(val & 0xc0000000) \| 8` (0x08493bb4). |

================================================================================
3. FIRMWARE & MICROCODE FINDINGS
================================================================================
Exhaustive analysis of the 64MB RAM dump (dumps/snap6/ram-full.bin), IRAM (iram.bin),
NAND reserved boot area (dumps/boot-area/reserved.bin), and BootROM/VROM images confirms:

1. NO Firmware or Microcode Upload:
   - There is NO microcode or DSP firmware uploaded to 0x38f00000.
   - The H.264 engine is a fixed-function hardware ASIC macroblock pipeline core, NOT a
     programmable DSP or Xtensa/RISC-V coprocessor.
   - The driver contains no file loading, no NAND stream calls, and no ROM-to-MMIO code
     transfers for the codec.

2. Driver Initialization Sequence (0x08232cfc):
   - Clock Enable: Writes `0x3e000004 |= 1` (SoC power management clock gate).
   - Interrupt Registration: Registers IRQ 35 (0x23) with handler 0x080df2d9.
   - Work Buffer Allocation: Allocates two kernel DMA scratch buffers:
     * Buffer 1: 0x4000 bytes (16 KB) via 0x08575784.
     * Buffer 2: 0x8000 bytes (32 KB) via 0x08575784.
     * Stored as physical page addresses shifted right by 10 (`bic 0x80000000 >> 10`).
   - Hardware Reset: 0x08495834 writes 0x38f01004 |= 0x7ff, polls bit 0 until 0.
   - Initial Register Backup (0x08496b18):
     * Hardware powers up with default hardware register values.
     * The driver reads 0x340 bytes from 0x38f00000, 0x900 bytes from 0x38f00400, and
       0x200 bytes from 0x38f01200 into RAM buffers.
     * On subsequent core resets (0x08496fa8), these default tables are written back to MMIO.
   - Interrupt Mask: Writes `0x38f010c8 = 0xffff`.
   - Clock / Gating: Writes `0x38f010d8 = (val & ~3) | 1`.
   - avcC Check: Validates profile (66 Baseline, 77 Main, 100 High) and level (<= 3.1).

================================================================================
4. DATAFLOW: BITSTREAM, YUV FRAMES, DRAM & CACHE
================================================================================
1. Input Bitstream (Demux -> DRAM -> DMA):
   - Container: MP4 / M4V (`avcC` atom).
   - Sample Format: 1, 2, or 4-byte big-endian NAL length prefixes followed by NAL data.
   - Delivery: The demuxer places the sample packet in guest DRAM.
   - Cache Maintenance: Before submission, 0x08232926 calls cache flush routine 0x0833528c
     (`r0=0x41, r1=0, r2=1`) to clean and invalidate CPU data cache lines.
   - Hardware Window: Driver sets base page (`+0x1200 = input_phys >> 10`), start offset
     (`+0x180c = input_phys & 0x3ff`), and end offset (`+0x1810 = start + NAL_length`).
   - Parsing: When `+0x1600` is programmed with `0x801`, hardware DMA fetches the NAL unit
     from DRAM, parses the NAL header, and writes status to `+0x1628`.
   - Exp-Golomb: Slices and parameters are parsed via `+0x1078` requests and syntax reads.

2. Output Framebuffers (DMA -> DRAM -> Display):
   - Surface Structure: 7-word descriptor allocated by 0x08232f9a:
     * +0x00: surface object pointer
     * +0x04: Y_phys >> 10 (Luma page pointer)
     * +0x08: chroma_phys >> 10 (Chroma page pointer)
     * +0x0c: aux_phys >> 10 (Aux / MV metadata buffer page pointer)
     * +0x10: Y_phys (physical address, bit 31 cleared)
     * +0x14: chroma_phys (physical address, bit 31 cleared)
     * +0x18: aux_phys (physical address, bit 31 cleared)
   - Pixel Format: **NV12** (semi-planar YUV 4:2:0):
     * Plane 0 (Luma): Contiguous Y plane at physical address `surface + 0x60`.
     * Plane 1 (Chroma): Contiguous interleaved UV plane (U0, V0, U1, V1...) at `surface + 0x6c`.
     * Aux Buffer: Temporal motion vector & deblocking scratchpad at `surface + 0x30`.
   - Slots: Driver maintains up to 20 DPB reference/output slots (i=0..19). Addresses are
     programmed into `+0x120c + 4*i` (Y), `+0x125c + 4*i` (UV), and `+0x12ac + 4*i` (Aux).
   - LCD Display Handoff: The LCD display controller at 0x38900000 reads directly from the
     decoded surface in DRAM: consumer 0x08080b70 calls `surface->getBuffer() & 0x7fffffff`
     and writes the frame address into `0x38900024`.

================================================================================
5. PRAGMATIC EMULATION APPROACH: HIGH-LEVEL EMULATION (HLE)
================================================================================
Full cycle-accurate register/bitstream modeling of an undocumented proprietary hardware
VLD/macroblock pipeline is infeasible and fragile. An HLE bridge backed by host libavcodec
(ffmpeg) is the proven, high-performance approach.

HLE Architecture:
```
+--------------------------------------------------------------------------+
| NanoEmu Guest OS (iPod nano 7G)                                          |
|                                                                          |
|   H264_DecoderDriver  --->  Programs MMIO @ 0x38f00000                   |
|   (Demux NALs, SPS/PPS)      - Bitstream bounds (+0x1200, +0x180c/10)     |
|                              - Surface slots (+0x120c, +0x125c, +0x12ac)  |
|                              - Trigger (+0x1000 = 1)                      |
|                                                                          |
|   IRQ 35 ISR         <---   Asserts IRQ 35, wakes semaphore 0x08811d44    |
|   Display Engine     <---   Reads decoded NV12 surface directly from DRAM |
+--------------------------------------------------------------------------+
                                    |
                                    v (QEMU MemoryRegionOps)
+--------------------------------------------------------------------------+
| NanoEmu QEMU H.264 HLE Device (`nano7_h264_real.c`)                      |
|                                                                          |
| 1. Parser Stub:                                                          |
|    - Intercepts writes to +0x1804 (DMA en) and +0x1600 (0x801 trigger).   |
|    - Reads NAL bytes from guest DRAM via cpu_physical_memory_read().     |
|    - Extracts `nal_unit_type` and `nal_ref_idc`.                          |
|    - Sets +0x1628: bit 8 = 1 (Ready), bits [4:0] = type, [6:5] = ref_idc. |
|    - Handles Exp-Golomb requests on +0x1078 by decoding UE/SE from NAL.  |
|                                                                          |
| 2. Bitstream Accumulator:                                                |
|    - Assembles SPS (type 7), PPS (type 8), and slice NALs (types 1, 5)    |
|      into an Annex-B stream buffer (`00 00 00 01 <NAL>`).                |
|                                                                          |
| 3. Host FFmpeg Decoder (libavcodec):                                     |
|    - On +0x1000 write (trigger decode):                                   |
|      a. `avcodec_send_packet()` with accumulated bitstream packet.        |
|      b. `avcodec_receive_frame()` -> AVFrame (NV12 or YUV420P).          |
|      c. Identify destination surface slot from active DPB index.          |
|      d. Extract target Y address (`slot_Y << 10`) and UV address         |
|         (`slot_UV << 10`).                                               |
|      e. DMA write decoded luma plane directly to guest DRAM at Y.         |
|      f. Interleave UV if needed (or copy directly if NV12) to UV in DRAM. |
|                                                                          |
| 4. Completion & IRQ Signaling:                                           |
|    - Set +0x1074 = 1 (decode success), clear error bits.                 |
|    - Ensure +0x10b8 bit 4 = 0 (no malformed error).                      |
|    - Call `qemu_set_irq(s->irq, 1)`.                                     |
|    - On guest write +0x10c0 = 0: `qemu_set_irq(s->irq, 0)` (deassert).   |
|    - On guest write-back to +0x1074: clear status word.                  |
+--------------------------------------------------------------------------+
```

================================================================================
6. TEST ASSET & PROOF OF SUCCESS PLAN
================================================================================
Real Test Asset Located on Work Machine:
  Path: `/home/pavliukov/ipodwork/music-db-proto/ipod-app/nanoapps/lvgl/tests/src/test_assets/test_video_birds.mp4`
  Specifications (verified with ffprobe):
    - Video Stream: H.264 High Profile (avc1), yuv420p, 432x240, 25.05 fps, 535 kbps.
    - Audio Stream: AAC-LC (mp4a), 44100 Hz, stereo, 160 kbps.
    - Container: ISO Media MP4 with standard 4-byte NAL length markers.
    - Compatibility: Resolution 432x240 exactly matches the iPod nano 7G landscape display
      (240x432 flipped 90 degrees for widescreen video playback). Profile 100 / Level 3.0
      satisfies the driver checks at 0x08232df2.

Testing & Verification Phases:
  Phase 1: Driver Initialization & Register Compliance
    - Boot NanoEmu with H264 device attached at 0x38f00000, IRQ 35.
    - Verify driver initializes without timeout:
      * 0x38f01004 reset poll completes with bit 0 == 0.
      * Work buffers (0x4000, 0x8000) accepted.
      * Register backup/restore ranges (0x000, 0x400, 0x1200) retained.
    - Success metric: No panic, no "Could not perform software reset" log message.

  Phase 2: NAL Parser Emulation Pass
    - Inject SPS / PPS and first IDR slice.
    - Verify +0x1628 returns Bit 8 = 1, NAL types 7, 8, 5, and valid `nal_ref_idc`.
    - Verify Exp-Golomb requests on +0x1078 return Bit 7 = 1 on +0x107c.
    - Success metric: No "ParsingStage() Parser error" or "Malformed video detected".

  Phase 3: Single-Frame Host Decode & DMA Writeback
    - On +0x1000 = 1, host libavcodec decodes frame 0 of `test_video_birds.mp4`.
    - Luma (Y) plane (432x240 bytes) and Chroma (UV) plane (432x120 bytes) written to
      guest DRAM at slot 0 addresses.
    - Status +0x1074 set to 1, IRQ 35 asserted.
    - Guest ISR acknowledges with +0x10c0 = 0, wakes semaphore, `decodeFrame()` returns 0.
    - Success metric: Frame 0 accepted by OS with return code 0.

  Phase 4: Full Playback, Visible Frames & Audio Sync
    - Launch video player in NanoEmu targeting `test_video_birds.mp4` synced via iTunes/Library.itdb.
    - LCD controller displays 432x240 video frames smoothly at 25 fps on emulator window.
    - Audio plays through existing I2S/DMA sound output.
    - Audio/Video Synchronization: Monitor PTS timestamps between audio DMA progress and
      video frame presentations; ensure drift remains < 40ms throughout the 20-second clip.
    - Proof of Success: Clean visible video frames rendered on screen in landscape mode with
      synchronized stereo AAC audio and zero kernel errors.
