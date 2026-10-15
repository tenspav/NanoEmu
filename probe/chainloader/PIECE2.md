# Piece 2: orderly hardware shutdown (`hw_shutdown_all`)

Spec: `docs/tasks/piece2-shutdown.md`. The code is in `shutdown.c` and `shutdown.h`.
Build check: `make -C probe/chainloader` (or the `shutdown-check` target alone). It compiles `shutdown.o` with the SDK app flags and `-Werror`. It then links the object on its own with `-nostdlib`, libgcc and `--no-undefined`. Any OS, SDK or libc reference fails the build. Result: 1020 bytes of code, 80 bytes of bss (the `hw_shutdown_last` log), no undefined symbols.

## Sources

The register choices come from two sources:
- **Plan:** `docs/research/dualboot-chainloader-plan.md`, section 4, phase 3 (hardware quiescence).
- **Device models:** the NanoEmu device models in `emulator/machine/hw/arm/`.

The models boot the real RetailOS, so where the two sources disagree, the model wins. Each conflict is marked **≠ plan** below.

## Order and behaviour

Before step a, the routine reads CPSR and runs `CPSID if` for the whole sequence. This stops OS ISRs from re-arming anything mid-shutdown: the audio DMA ring, TE frame pushes and the timer tick.

| Step | What it does | Wait bound |
|---|---|---|
| a. Display | Sets the OS quiesce flag, masks and acks the TE interrupt, waits out an in-flight DSI push, waits out a running blit, then masks and acks the blitter IRQ. | 50 ms + 50 ms |
| b. FMC/NAND | Masks the FMSS IRQ, then lets in-flight work settle (it exits early once the controller reads idle). Then it aborts polling, deselects the chips and W1C-acks both status registers. | 20 ms |
| c. DMA | For both engines and all 8 channels: NEXT=0, wait for the run bit to drop, abort any channel still running, then W1C-ack. | 50 ms total |
| d. Timers | For all 8 channels: CMD=0, clear the IRQ enables, clear pending (in CON for channels A–D, in TSTAT for E–H). | none |
| e. VIC | Saves INTENABLE, then INTENCLEAR all lines, SOFTINTCLEAR, EOI and clear the glue latches. It reads INTENABLE back and runs `CPSID if` again. | none |

**Timeouts.** All timeouts use the timer block's free-running 1 MHz counter (`0x3c700084`). Step d does not stop that counter. If the counter is frozen, a loop-count backstop ends the wait and sets `HW_SD_NO_CLOCK`.

**Failures.** A timeout never aborts the shutdown. It only sets a flag:
- The routine returns a flag word (`HW_SD_*` in `shutdown.h`).
- It also fills `hw_shutdown_last` with what each step saw: entry and exit CPSR, FMC status before the ack, DMA channels that were active or aborted, running timers, and VIC enables before and after.
- `HW_SD_NOT_PRIV` means the routine ran in user mode, where CPS is a no-op. The MMIO writes still landed, but IRQs are on.

## Registers written

**Display**
| Address | Value | Purpose | Source |
|---|---|---|---|
| `[0x08ae642c] + 0x772` (DRAM byte) | `1` | OS display-object quiesce flag: the TE callback (`0x080755b4`) then skips both commit and DSI push. Only written if the pointer lies in DRAM. | `docs/research/direct-display-path.md` §183-188. The plan only says "quiesce LCD scanout" and gives no register. |
| `0x397000c4` (GPIO IRQ group 1 mask) | read-modify-write, clears bit 6 | Masks the panel TE interrupt (GPIO 14, VIC IRQ 32). | `nano7_gpio.c` header (group regs `+0xc0+4g`); `nano7.c:173` (OS arms bit 6 here). **≠ `lcd.md:123`**, which says enable `0x397000a4` and status `0x39700084`. |
| `0x397000a4` (GPIO IRQ group 1 pending) | `0x40` | W1C: drops a latched TE edge. | `nano7_gpio.c` (`+0xa0+4g` pending, W1C). |
| `0x39000008` (blitter IRQ enable) | `0` | Disables frame-done IRQ 37. | `nano7_disp.c` header; `nano7.c:163`. |
| `0x3900000c` (blitter IRQ status) | `1` | W1C: acks frame done. | `nano7_disp.c` header. |

**Display, read only**
| Address | Wait condition | Source |
|---|---|---|
| `0x3830008c` | bits 1:0 = 0 (panel push idle) | `lcd.md:130-131` |
| `0x39000000` | bit 1 = 0 (blit not running) | `nano7_disp.c` |

**FMC/NAND** (base `0x38a00000`, VIC IRQ 54)
| Address | Value | Purpose | Source |
|---|---|---|---|
| `0x38a00c10` (FMSS_IRQEN) | `0` | Sequencer IRQ off. The routine writes this before waiting. | Plan; `nano7_fmc.c:57,763-767` |
| `0x38a00004` (FMC_CTRL) | `0x20` | Aborts status polling or a PIO data operation. It does not start a NAND cycle. | Plan; `nand-fmc.md:46` |
| `0x38a00000` (FMC_CS_CFG) | `1` | Deselects all chips (bus idle). | Plan; `nand-fmc.md` (+0x00); `nano7_fmc.c:694-696` |
| `0x38a00048` (FMC_INT_STATUS) | `0xffffffff` | W1C: acks phase-done flags. The routine writes this after the abort, because the abort can set those flags. | Plan; `nano7_fmc.c:754-756` |
| `0x38a00c0c` (FMSS_CS_STATUS) | `0xffffffff` | W1C: acks sequencer done/error. | Plan; `nano7_fmc.c:757-762` |

**FMC/NAND, read only**
| Address | Wait condition | Source |
|---|---|---|
| `0x38a00048` | `& 0x0181000e` = 0 | Plan (same test as DumpRAM/RAMScope) |
| `0x38a00c0c` | = 0 | Plan (same test as DumpRAM/RAMScope) |

**FMC/NAND, never written.** These registers start NAND operations (`nano7_fmc.c` analysis), so the routine never writes them:
- `+0xc00`: FMSS kick
- `+0x08`: command latch
- `+0x34`: data DMA
- `+0x0c`, `+0x10`, `+0x14`, `+0x30`

**DMA** (engines `0x38200000` on IRQ 16 and `0x38700000` on IRQ 17; channel n = 0..7)
| Address | Value | Purpose | Source |
|---|---|---|---|
| `base + 0x108 + 0x20n` (NEXT) | `0`, then read back | The current pass becomes the last one. This breaks the self-linked audio ring on engine 1 channel 2. | Plan; `nano7_dma.c` header and `:203,228` |
| `base + 0x110 + 0x20n` (CTRL) | `CTRL & ~1` | Abort. Only written for a channel still running after the 50 ms drain. | Plan; `nano7_dma.c:230-247` (OS stop routine `0x080ba2a4`) |
| `base + 0x08` (ACK) | `0xffffffff` | W1C: clears the completion bits and drops the IRQ. | `nano7_dma.c:216-219`. **≠ plan**, which says the status register at +0x08 is W1C. In fact the status is read-only at +0x04 and the ack is at +0x08. |

The DMA block has no mask register and no global enable (`nano7_dma.c`). The IRQ line is simply asserted while status ≠ 0.

**DMA, read only**
| Address | Read for | Source |
|---|---|---|
| `base + 0x110 + 0x20n` | bit 0 = run, polled until clear | `nano7_dma.c` |
| `base + 0x04` | completion status, logged after the ack | `nano7_dma.c:194-195` |

**Timers** (base `0x3c700000`, all channels on VIC IRQ 8; channel offsets c ∈ {0, 0x20, 0x40, 0x60, 0xa0, 0xc0, 0xe0, 0x100})
| Address | Value | Purpose | Source |
|---|---|---|---|
| `0x3c700000 + c + 0x04` (CMD) | `0` | Stops the channel. The OS tick is channel B (`+0x20`, 500 Hz). | Plan; `nano7_timer.c:234-249`, header lines 16-18 |
| `0x3c700000 + c + 0x00` (CON), channels A–D | `(CON & ~0x77000) \| 0x70000` | Clears IRQ enables 14:12 and W1C-clears pending 18:16. | `nano7_timer.c:224-233`. **≠ plan**, which only clears 0x7000. Pending bits for A–D live in CON, not TSTAT, and `CMD=0` does not drop them. |
| `0x3c700000 + c + 0x00` (CON), channels E–H | `CON & ~0x7000` | Clears the IRQ enables. | Plan; `nano7_timer.c:224-233` |
| `0x3c700118` (TSTAT) | `0x07070707` | W1C: pending for channels E–H. | Plan; `nano7_timer.c:100-103,208-212` |

**Timers, read only**
| Address | Read for | Source |
|---|---|---|
| `0x3c700084` | free-running 1 MHz counter, used for timeouts | `nano7_timer.c:167-172` |

**VIC** (VIC0 `0x38e00000` for IRQ 0–31, VIC1 `0x38e01000` for IRQ 32–63)
| Address | Value | Purpose | Source |
|---|---|---|---|
| `0x38e00014`, `0x38e01014` (INTENCLEAR) | `0xffffffff` | Masks every line, IRQ and FIQ. | Plan; `nano7_vic.c:201` |
| `0x38e0001c`, `0x38e0101c` (SOFTINTCLEAR) | `0xffffffff` | Clears software interrupts. | Plan; `nano7_vic.c:203` |
| `0x38e00f00`, `0x38e01f00` (VICADDRESS) | `0x01010101` | EOI for any in-service vector. This is the value the OS writes after each IRQ. | Plan; `nano7_vic.c` header |
| `0x38e02008`, `0x38e0200c` (glue edge clear) | `0xffffffff` | Clears the per-bank edge latches. The OS init writes this same value. | Plan; `nano7_vic.c` header lines 8-9 |

**VIC, read only**
| Address | Read for | Source |
|---|---|---|
| `0x38e00010`, `0x38e01010` | INTENABLE before and after; 0 is expected after | `nano7_vic.c:197-200` |

## Open questions

1. **No real FMC busy bit.** `+0x48 & 0x0181000e` are phase-*done* flags (`nand-fmc.md`), not busy flags. With IRQs off, nobody acks them, so a latched flag makes the idle test time out after 20 ms (`HW_SD_FMC_PENDING`). That is harmless, because the flag means the operation already completed. The 20 ms settle exists so that a program or erase that was already confirmed can finish inside the chip.
   - A true ready/busy indicator (a chip R/B pin register, or FMSS "running") is unknown.
   - Because of this, the caller must wait for FMC idle while the OS is still alive, before calling: close files, then run DumpRAM's `nand_wait_idle`.
   - NanoEmu cannot test this: its FMSS runs synchronously inside the kick.
2. **TE register addresses.** `lcd.md` and `nano7_gpio.c` disagree. The routine follows the model: mask at `0x397000c4`, pending at `0x397000a4`. The model boots the OS with TE working, but a device check (read `0x39700084/a4/c4` in a probe) would settle it.
3. **The display quiesce flag is OS-version specific.** `0x08ae642c`/`+0x772` belong to RetailOS 1.1.2. The pointer is range-checked. With IRQs off and TE masked, the flag only matters for a user-mode caller.
4. **NanoEmu keeps compositing from DRAM.** `nano7_lcd.c` re-reads the LCD windows from DRAM on every 60 Hz tick, whatever the guest does, unlike the real command-mode panel, whose GRAM holds the last frame. Once the payload overwrites the framebuffer's DRAM, the emulated screen will change while the device's would not. A launcher test that expects "last frame stays up" needs a model change, for example freezing the scanout once VIC masks every line or the TE GPIO is masked. That change was not made here.
5. **LCD controller registers are untouched.** The routine never writes `0x38900000` (`+0x00`, `+0x04` bits 4/5/8, `+0x1b10`, IRQ enable `+0x08 = 0x700`). On the device, nobody knows whether stopping the timing generator blanks the panel. The emulator never raises LCD IRQ 13, and the VIC masks it anyway.
6. **Glue latch semantics** (`0x38e02008/0c`) are inferred from the OS's own writes. The model just stores them.
7. **Blocks the plan names but the spec leaves out:** MBX GPU (`0x3b000000`), ADSP (`0x38500000`), USB OTG (`0x38400000`), and the watchdog (`0x3c800000`).
   - The GPU and ADSP are bus masters. If a render job or audio DSP is active at hand-over, they could write DRAM after relocation. The launcher should exit GL apps and stop playback first, or a later piece should add those steps.
   - The watchdog is left as the OS set it. The payload must kick it or rely on it, as `payload_hello` does.
8. **Interrupts are masked at entry, not only at the end.** The spec says the routine "returns with CPSID if". It also masks at entry, so no ISR can run between steps. Steps a–d do not depend on any ISR.
9. **Untested in NanoEmu.** This piece is compile-checked only. The flag word and `hw_shutdown_last` are there for the integration run, for example dumping 80 bytes at `&hw_shutdown_last` via QMP after the hand-over.
