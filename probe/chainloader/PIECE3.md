# Piece 3: SRAM Hand-off Stub & CPU Takeover (`handoff.S`, `handoff.h`, `chainloader.c`)

This document accompanies the SRAM hand-off implementation in `probe/chainloader/` (`handoff.S`, `handoff.h`, `chainloader.c`, `Makefile`), fulfilling the requirements of `docs/tasks/piece3-handoff.md` and the architecture in `docs/research/dualboot-chainloader-plan.md`.

---

## 1. Overview & System Role

Piece 3 is the assembly bridge between orderly hardware shutdown (`hw_shutdown_all()` from Piece 2) and bare-metal payload execution (`payload_hello.c` from Piece 1, or subsequent Linux/Rockbox kernels).

```
+-------------------------------------------------------------------------+
| RetailOS User Space (chainloader.c)                                     |
|  1. Find & read payload.bin from FAT32 into DRAM staging buffer        |
|  2. If absent, use statically embedded default_payload_bin             |
|  3. Call hw_shutdown_all() [Piece 2: stops LCD, FMC, DMA, Timers, VIC] |
|  4. Call handoff_to_payload(payload, size) [Piece 3 entry]             |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| handoff_to_payload (handoff.S, DRAM)                                   |
|  1. Copy position-independent trampoline to SRAM 0x22000000             |
|  2. Copy warm-reset trap stub to SRAM 0x22008100                        |
|  3. Populate struct n7_handoff ABI block at SRAM 0x22009000            |
|  4. Hook RetailOS SVC dispatch table slot 0 at 0x08917714               |
|  5. Trigger svc #70 (or direct branch if already in SVC mode)          |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| Takeover Trampoline (ARM state, SVC mode, executing from SRAM 0x22000000)|
|  1. CPSID aif: disable Asynchronous aborts, IRQs, FIQs in CPSR          |
|  2. Preserve arguments in registers r8..r10 (ZERO stack access)         |
|  3. Clean & invalidate L1 D-Cache by Set/Way to Point of Coherency      |
|  4. DSB                                                                 |
|  5. Clear SCTLR bits: M (MMU), C (D-Cache), Z (Branch), I (I-Cache), V  |
|  6. Invalidate I-Cache (ICIALLU), Branch Predictor (BPIALL), TLB        |
|  7. DSB; ISB                                                            |
|  8. Relocate payload from DRAM staging buffer to physical 0x08000000    |
|  9. DSB; ISB                                                            |
| 10. Establish Linux/start.S register ABI:                               |
|       r0 = 0                                                            |
|       r1 = 0x00001097 (Machine Architecture ID)                         |
|       r2 = 0x22009000 (Pointer to struct n7_handoff in SRAM)            |
|       sp = 0x22010000 (Private SRAM stack top)                          |
|       lr = 0x22008100 (Warm reset trap handler)                        |
| 11. Branch: bx 0x08000000                                               |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| Bare-Metal Payload (_start @ physical DRAM 0x08000000)                  |
|  - Exclusive flat physical control of all 64 MiB DRAM                   |
+-------------------------------------------------------------------------+
```

---

## 2. Memory Map Used

### A. Internal SRAM (`0x22000000` – `0x22040000`, 256 KiB)

Internal SRAM is on-chip volatile memory that is independent of the external DRAM controller and FMC NAND bus. Its physical address is `0x22000000` and it is identity-mapped (Virtual Address = Physical Address = `0x22000000`, L1 descriptor `0x22084c02` in RetailOS).

| Address Range | Size | Component | Description |
|---|---|---|---|
| `0x22000000` – `0x22000124` | 292 B | Takeover Trampoline (`_trampoline_src`) | Position-independent ARM code: cache clean, MMU off, payload copy, ABI setup, branch. |
| `0x22000124` – `0x22008100` | ~32 KiB | Free SRAM | Reserved / unused headroom. |
| `0x22008100` – `0x2200812c` | 44 B | Warm Reset Stub (`_reset_stub_src`) | Trap handler targeted by payload `lr`; reloads watchdog and triggers PMGR reset on return. |
| `0x2200812c` – `0x22009000` | ~3.7 KiB | Free SRAM | Headroom. |
| `0x22009000` – `0x22009028` | 40 B | Handoff Block (`struct n7_handoff`) | ABI structure passed in `r2` (magic `"N7CL"`, DRAM base, size, entry, MMIO pointers). |
| `0x22009028` – `0x22010000` | ~28 KiB | Private Takeover Stack | Grows downwards from `0x22010000`. |
| `0x22010000` – `0x22040000` | 192 KiB | Upper SRAM | Available for future secondary stages (e.g. ATAGS / devicetree). |

### B. System DRAM (`0x08000000` – `0x0c000000`, 64 MiB)

DRAM is 1:1 identity-mapped across all 64 1-megabyte sections in RetailOS.

| Address Range | Size | Component | Description |
|---|---|---|---|
| `0x08000000` – `0x08000130` | 304 B | Payload Code/Data | Target destination where payload binary is relocated. |
| `0x08000130` – `0x08004130` | 16 KiB | Payload BSS / Stack | Private stack established by `start.S`. |
| `0x08200000` – `0x08265400` | 405 KiB | Fallback Framebuffer | 240x432x4 ARGB8888 panel buffer. |
| `0x08917714` | 4 B | SVC Dispatcher Table Slot 0 | Original target of `svc #70, r12=0` (`0x080e04c7`). Hooked with `0x22000000`. |
| `0x09280000` | ~2.5 KiB | Chainloader Binary | Launcher code in RetailOS high-RAM app window. |
| `0x09500000`+ | 1 MiB | `s_staging_buffer` | DRAM staging buffer holding the loaded `payload.bin` prior to takeover. |

---

## 3. RetailOS Supervisor Escalation via `0x08917714`

### A. The Mechanism
RetailOS user-mode applications run in User mode (`CPSR = 0x10`) but have unrestricted write access to DRAM page tables and kernel dispatch tables.

The RetailOS kernel handles `svc #70` (`0x46`) through the exception path at `0x082f3828`:
1. Vector entry: reads the Thumb immediate byte (`#70`), preserves `r0..r3`, and branches to `0x08002390`.
2. Dispatcher `0x08002390`:
   ```arm
   8002390: b430          push {r4, r5}
   8002392: 4c02          ldr  r4, =0x08917714    ; Dispatch table base
   8002394: f854 3023     ldr.w r3, [r4, r3, lsl #2] ; Load table entry for index (r3 = ip / r12)
   8002398: bc30          pop  {r4, r5}
   800239a: 4718          bx   r3                 ; Jump to handler in SVC mode!
   ```
3. Original table slot 0 (`0x08917714`): points to `0x080e04c7` (the OS `irq_save` routine).
4. Privilege escalation hook:
   - Write `0x22000000` to `*(volatile uint32_t *)0x08917714`.
   - Issue `DSB` and `ISB`.
   - Invoke `svc #70` with `r12 = 0`.
   - The kernel takes the SVC exception, elevates CPSR to `0x13` (Supervisor), executes `0x08002390`, loads `0x22000000` from slot 0, and branches to `0x22000000` via `bx r3`.
   - Because bit 0 of `0x22000000` is 0, `bx` automatically transitions the processor from Thumb state to 32-bit ARM state.
   - Arguments in `r0` (payload pointer) and `r1` (payload size) are preserved intact through the SVC gateway.

### B. Dual-Mode Fallback
If `handoff_to_payload` is invoked when the CPU is already in SVC mode (e.g. from a kernel context or unit test), it reads CPSR (`mrs r2, cpsr`), checks `r2 & 0x1f == 0x13`, and directly executes `bx 0x22000000` without triggering a recursive exception.

---

## 4. Exact CP15 Sequence & Register Values

The sequence is executed strictly from internal SRAM at `0x22000000` with registers only (no stack operations).

### Step 1: Interrupt Masking
```arm
cpsid aif
```
- **Action:** Sets bits A (bit 8, Asynchronous abort mask), I (bit 7, IRQ mask), and F (bit 6, FIQ mask) in CPSR.
- **Why:** Although Piece 2 (`hw_shutdown_all`) masked interrupts at the VIC controller level, setting CPSR bits directly in the CPU core ensures no pending asynchronous exceptions or interrupts can fire during cache teardown and MMU disablement.

### Step 2: L1 Data Cache Clean and Invalidate by Set/Way
```arm
dsb sy
mov r0, #0
mcr p15, 2, r0, c0, c0, 0       /* CSSELR: Select Level 1 Data Cache */
isb sy
mrc p15, 1, r0, c0, c0, 0       /* CCSIDR: Read Cache Size ID Register */
```
- **CSSELR (Cache Size Selection Register):**
  - Value: `0` (bits [3:1] = `000` for Level 1, bit 0 = `0` for Data/Unified cache).
- **CCSIDR (Cache Size ID Register):**
  - Read value on Cortex-A5 / Cortex-A7 (NanoEmu):
    - Bits [2:0] (LineSize): `1` $\implies$ line size is $2^{1+2} = 8$ words = 32 bytes ($L = 5$).
    - Bits [12:3] (Associativity): `3` $\implies 4$ ways ($A = 2$, way shift $32 - A = 30$).
    - Bits [27:13] (NumSets): `127` $\implies 128$ sets.
- **Loop Logic:**
  ```arm
  .Ltramp_way_loop:
      mov     r6, r5              /* r6 = 127 down to 0 (sets) */
  .Ltramp_set_loop:
      lsl     r7, r3, r4          /* Way (0..3) shifted to bits [31:30] */
      lsl     r11, r6, r2         /* Set (0..127) shifted to bits [11:5] */
      orr     r7, r7, r11         /* Combine: level 0 is in bits [3:1] = 0 */
      mcr     p15, 0, r7, c7, c14, 2  /* DCCISW */
      subs    r6, r6, #1
      bge     .Ltramp_set_loop
      subs    r3, r3, #1
      bge     .Ltramp_way_loop
  ```
- **MCR DCCISW (`c7, c14, 2`):**
  - Parameter `<Rt>`: `(Way << 30) | (Set << 5) | (Level << 1)`.
  - **Why:** Cleans (writes back dirty cache lines to DRAM) and invalidates all 512 lines in the data cache. Without this, newly written payload data sitting in dirty cache lines would be lost upon disabling the cache, or stale cache entries would cause coherency faults when the MMU is disabled.

### Step 3: Data Synchronization Barrier
```arm
dsb sy
```
- **Why:** Guarantees all data writebacks from DCCISW to Point of Coherency (PoC / DRAM) complete across the internal bus before modifying system control registers.

### Step 4: MMU, Caches & Branch Prediction Disable (SCTLR)
```arm
mrc p15, 0, r0, c1, c0, 0       /* Read SCTLR */
movw r1, #0x3805
bic r0, r0, r1                  /* Clear M, C, Z, I, V */
mcr p15, 0, r0, c1, c0, 0       /* Write SCTLR */
```
- **SCTLR (System Control Register, CP15 c1, c0, 0):**
  - Bits cleared by mask `0x3805`:
    - **Bit 0 (`M = 0`):** Disables the MMU. Virtual addresses become 1:1 physical addresses.
    - **Bit 2 (`C = 0`):** Disables Data and Unified caches. All memory accesses bypass cache.
    - **Bit 11 (`Z = 0`):** Disables Branch Prediction. Prevents speculative execution on stale branch targets.
    - **Bit 12 (`I = 0`):** Disables Instruction cache.
    - **Bit 13 (`V = 0`):** Selects normal (low) exception vectors (`0x00000000` / `VBAR`) rather than high vectors (`0xffff0000`).
- **Why:** Turns off virtual translation and caching safely while executing from internal SRAM.

### Step 5: Invalidate Instruction Cache, Branch Predictor, and TLB
```arm
mov r0, #0
mcr p15, 0, r0, c7, c5, 0       /* ICIALLU: Invalidate all instruction caches */
mcr p15, 0, r0, c7, c5, 6       /* BPIALL: Invalidate branch predictor array */
mcr p15, 0, r0, c8, c7, 0       /* TLBIALL: Invalidate entire unified TLB */
```
- **Why:**
  - `ICIALLU`: Flushes any prefetched instructions fetched under the old MMU mapping.
  - `BPIALL`: Clears branch target history so speculative branches do not jump to stale targets.
  - `TLBIALL`: Flushes all translation lookaside buffers so no cached page table entries remain active.

### Step 6: Pipeline and Memory Synchronization
```arm
dsb sy
isb sy
```
- **Why:**
  - `DSB`: Ensures completion of TLB and cache invalidation operations.
  - `ISB`: Flushes the processor instruction pipeline so that all instructions fetched after this point are decoded using physical addressing and disabled caches.

### Step 7: Relocate Payload to Physical DRAM `0x08000000`
```arm
    cmp     r8, sl              /* If source == 0x08000000, skip copy */
    beq     .Ltramp_copy_done
    mov     r0, sl              /* dest = 0x08000000 */
    mov     r1, r8              /* src = payload staging buffer */
    mov     r2, r9              /* size in bytes */
.Ltramp_copy_words:
    cmp     r2, #4
    blo     .Ltramp_copy_bytes
    ldr     r3, [r1], #4
    str     r3, [r0], #4
    sub     r2, r2, #4
    b       .Ltramp_copy_words
.Ltramp_copy_bytes:
    cmp     r2, #0
    beq     .Ltramp_copy_done
    ldrb    r3, [r1], #1
    strb    r3, [r0], #1
    sub     r2, r2, #1
    b       .Ltramp_copy_bytes
.Ltramp_copy_done:
    dsb     sy
    isb     sy
```
- **Why:** Copies the payload binary down from high DRAM staging (`0x09500000`+) to its linked physical execution base at `0x08000000`. Overlap-safe forward copy handles arbitrary sizes.

### Step 8: Establish Handoff Register ABI & Jump
```arm
mov     r0, #0
movw    r1, #0x1097
movt    r1, #0x0000             /* r1 = 0x00001097 */
movw    r2, #0x9000
movt    r2, #0x2200             /* r2 = 0x22009000 */
mov     r3, #0
mov     r4, #0
mov     r5, #0
mov     r6, #0
mov     r7, #0
movw    sp, #0x0000
movt    sp, #0x2201             /* sp = 0x22010000 */
movw    lr, #0x8100
movt    lr, #0x2200             /* lr = 0x22008100 */
mov     ip, #0x08000000         /* r12 = entry point */
bx      ip                      /* Jump to payload! */
```
- **Register Convention:**
  - `r0 = 0`: Standard ARM Linux kernel boot convention.
  - `r1 = 0x00001097`: Samsung S5L8747 / iPod nano 7G machine architecture ID.
  - `r2 = 0x22009000`: Address of `struct n7_handoff` in SRAM.
  - `sp = 0x22010000`: Safe private SRAM stack.
  - `lr = 0x22008100`: Points to the SRAM warm reset stub. If `payload_main()` returns via `bx lr`, the reset stub triggers a system restart back to stock RetailOS.
  - `CPSR`: ARM state, SVC mode (`0x13`), A/I/F interrupts disabled (`0x000001d3`).

---

## 5. Byte-Exact Trampoline Listing (Disassembly Verification)

The trampoline contains **zero** literal pool references, **zero** relocations (`R_ARM_ABS32 = 0`), and is **non-self-modifying**. All relative branches are intra-trampoline.

```
Address   Opcode     Instruction                    Comment
-------   --------   ---------------------------    --------------------------------------------
0x0000    f10c01c0   cpsid   aif                    Mask all interrupts (A, I, F)
0x0004    e1a08000   mov     r8, r0                 r8 = payload source address
0x0008    e1a09001   mov     r9, r1                 r9 = payload size in bytes
0x000c    e3a0a302   mov     sl, #0x08000000        r10 = DRAM destination base
0x0010    f57ff04f   dsb     sy                     Data Synchronization Barrier
0x0014    e3a00000   mov     r0, #0                 Select level 0 data cache
0x0018    ee400f10   mcr     p15, 2, r0, c0, c0, 0  Write CSSELR
0x001c    f57ff06f   isb     sy                     Instruction Synchronization Barrier
0x0020    ee300f10   mrc     p15, 1, r0, c0, c0, 0  Read CCSIDR (cache geometry)
0x0024    e2002007   and     r2, r0, #7             LineSize bits [2:0]
0x0028    e2822004   add     r2, r2, #4             r2 = line length shift (L)
0x002c    e30033ff   movw    r3, #0x03ff            Ways mask
0x0030    e00331a0   and     r3, r3, r0, lsr #3     r3 = number of ways - 1
0x0034    e16f4f13   clz     r4, r3                 r4 = way shift (32 - log2(ways))
0x0038    e3075fff   movw    r5, #0x7fff            Sets mask
0x003c    e00556a0   and     r5, r5, r0, lsr #13    r5 = number of sets - 1
0x0040    e1a06005   mov     r6, r5                 r6 = current set index
0x0044    e1a07413   lsl     r7, r3, r4             Way in bits [31:30]
0x0048    e1a0b216   lsl     fp, r6, r2             Set in bits [11:5]
0x004c    e187700b   orr     r7, r7, fp             Combine into DCCISW set/way format
0x0050    ee077f5e   mcr     p15, 0, r7, c7, c14, 2 Clean & Invalidate line by Set/Way
0x0054    e2566001   subs    r6, r6, #1             Decrement set index
0x0058    aafffff9   bge     0x0044                 Loop sets
0x005c    e2533001   subs    r3, r3, #1             Decrement way index
0x0060    aafffff6   bge     0x0040                 Loop ways
0x0064    f57ff04f   dsb     sy                     Wait for writebacks to reach PoC
0x0068    ee110f10   mrc     p15, 0, r0, c1, c0, 0  Read SCTLR
0x006c    e3031805   movw    r1, #0x3805            Bits to clear: M, C, Z, I, V
0x0070    e1c00001   bic     r0, r0, r1             Clear bits in r0
0x0074    ee010f10   mcr     p15, 0, r0, c1, c0, 0  Write SCTLR (MMU & Caches OFF)
0x0078    e3a00000   mov     r0, #0
0x007c    ee070f15   mcr     p15, 0, r0, c7, c5, 0  ICIALLU: Invalidate I-Cache
0x0080    ee070fd5   mcr     p15, 0, r0, c7, c5, 6  BPIALL: Invalidate Branch Predictor
0x0084    ee080f17   mcr     p15, 0, r0, c8, c7, 0  TLBIALL: Invalidate Unified TLB
0x0088    f57ff04f   dsb     sy                     Barrier
0x008c    f57ff06f   isb     sy                     Instruction pipeline flush
0x0090    e158000a   cmp     r8, sl                 Check if source == 0x08000000
0x0094    0a00000e   beq     0x00d4                 Skip copy if already in place
0x0098    e1a0000a   mov     r0, sl                 r0 = destination (0x08000000)
0x009c    e1a01008   mov     r1, r8                 r1 = source
0x00a0    e1a02009   mov     r2, r9                 r2 = byte count
0x00a4    e3520004   cmp     r2, #4                 Word copy loop
0x00a8    3a000003   bcc     0x00bc
0x00ac    e4913004   ldr     r3, [r1], #4           Read word
0x00b0    e4803004   str     r3, [r0], #4           Write word
0x00b4    e2422004   sub     r2, r2, #4
0x00b8    eafffff9   b       0x00a4
0x00bc    e3520000   cmp     r2, #0                 Byte copy loop (trailing bytes)
0x00c0    0a000003   beq     0x00d4
0x00c4    e4d13001   ldrb    r3, [r1], #1           Read byte
0x00c8    e4c03001   strb    r3, [r0], #1           Write byte
0x00cc    e2422001   sub     r2, r2, #1
0x00d0    eafffff9   b       0x00bc
0x00d4    f57ff04f   dsb     sy                     Memory barrier
0x00d8    f57ff06f   isb     sy                     Instruction sync
0x00dc    e3a00000   mov     r0, #0                 ABI r0 = 0
0x00e0    e3011097   movw    r1, #0x1097            ABI r1 = 0x00001097
0x00e4    e3401000   movt    r1, #0
0x00e8    e3092000   movw    r2, #0x9000            ABI r2 = 0x22009000 (handoff block)
0x00ec    e3422200   movt    r2, #0x2200
0x00f0    e3a03000   mov     r3, #0                 Zero r3..r7
0x00f4    e3a04000   mov     r4, #0
0x00f8    e3a05000   mov     r5, #0
0x00fc    e3a06000   mov     r6, #0
0x0100    e3a07000   mov     r7, #0
0x0104    e300d000   movw    sp, #0x0000            sp = 0x22010000
0x0108    e342d201   movt    sp, #0x2201
0x010c    e308e100   movw    lr, #0x8100            lr = 0x22008100 (reset handler)
0x0110    e342e200   movt    lr, #0x2200
0x0114    e3a0c302   mov     ip, #0x08000000        r12 = 0x08000000
0x0118    e12fff1c   bx      ip                     Branch to bare-metal payload!
0x011c    e320f003   wfi                            Dead loop
0x0120    eafffffd   b       0x011c
```

---

## 6. Piece 4 App Glue (`chainloader.c`)

`chainloader.c` binds the entire pipeline together into a runnable RetailOS homebrew app:
1. **Payload Search & Staging:**
   Iterates through candidate paths:
   - `/Apps/DualBoot/payload.bin`
   - `/Apps/Chainloader/payload.bin`
   - `/Apps/Startup/payload.bin`
   - `payload.bin`
   If an on-disk payload is present, it checks `hb_fs_size()`, reads it into DRAM buffer `s_staging_buffer` (1 MiB headroom).
   If no file is present on the FAT32 filesystem, it falls back seamlessly to `default_payload_bin` (the 304-byte `payload.bin` built in Piece 1 and linked into `handoff.o`).
2. **Quiesce:** Calls `hw_shutdown_all()`.
3. **Takeover:** Calls `handoff_to_payload(payload_ptr, payload_len)`.
4. **Linker compatibility:** Exposes `payload_entry` (for relocatable `.hbapp` bundles), `main` (for freestanding binary builds), and `hb_raw_init` (for raw surface compatibility).

---

## 7. Assumptions & Constraints

1. **Physical DRAM Identity Mapping:** RetailOS maps all 64 MiB of DRAM identity (`VA == PA == 0x08000000..0x0c000000`). Staging addresses in DRAM remain identical when the MMU is disabled.
2. **SRAM Identity Mapping:** SRAM (`0x22000000`) is mapped identity in user space with full Read/Write/Execute permissions. The trampoline execution continues smoothly as SCTLR bit 0 drops.
3. **No NAND Writes:** At no point does the chainloader or trampoline write to NAND flash. The NAND controller is placed in an idle/deselected state with sequencer IRQs disabled.
4. **Reversible Execution:** Returning from payload via `bx lr` enters the trap handler at `0x22008100` and resets the device via PMGR `0x3c500050 = 0xa5`, restoring the stock OS cleanly.

---

## 8. Build Verification

Build command:
```bash
make -C probe/chainloader
```

Artifacts verified:
- `build/payload.elf` + `payload.bin`: 304-byte bare-metal payload (Piece 1).
- `build/shutdown-check.elf`: 1020-byte orderly hardware shutdown engine, zero undefined symbols (Piece 2).
- `build/handoff-check.elf`: 620-byte standalone SRAM hand-off engine, zero undefined symbols (Piece 3).
- `build/chainloader.elf` + `build/chainloader.bin`: 2384-byte full launcher executable with SDK filesystem reader, zero undefined symbols.
- `build/chainloader.hbapp`: 2468-byte relocatable bundle packaged by `mkrelocapp.py`, ready for disk injection into NanoEmu or hardware untether.
