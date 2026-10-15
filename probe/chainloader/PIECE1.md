# Piece 1: Bare-Metal Hello Payload Implementation Notes

This document accompanies the bare-metal payload in `probe/chainloader/` (`payload_hello.c`, `start.S`, `payload.ld`, `Makefile`), implementing the requirements of `docs/tasks/piece1-payload.md`.

---

## 1. Overview & Execution Context

The payload is a tiny standalone bare-metal program for the iPod nano 7G (Samsung S5L8747, ARM Cortex-A5 core). It is designed to be loaded by the chainloader at physical DRAM address `0x08000000` after the system has quiesced hardware engines (FMC, DMA, Timers, VIC), transitioned to IRAM execution, and disabled the Cortex-A5 MMU, caches, and interrupts.

On execution:
1. Entry point `_start` establishes a private stack in DRAM and clears the BSS section.
2. It configures LCD Window 0 and fills the entire 240x432 32-bit framebuffer with solid bright green (`0x00FF00`).
3. It polls the Home button via GPIO MMIO.
4. When the Home button is pressed, it triggers a system reset to return the hardware to stock RetailOS.

---

## 2. Hardware Registers Used & Sources

### A. LCD Controller (Base: `0x38900000`)

| Register | Offset | Value / Bitfield | Description |
|---|---|---|---|
| `REG_LCD_ENABLE` | `0x38900004` | `bit 4 = 1` (`0x10`) | Window 0 enable flag (`bit 5` is Window 1) |
| `REG_LCD_WIN0_CTL` | `0x38900020` | `0x002107ff` | Format 7 (ARGB8888, bits 11:8), alpha `0xff` (bits 7:0), layer flags `bit 16` and `bit 21` |
| `REG_LCD_WIN0_BASE` | `0x38900024` | DRAM physical address (`0x08200000` or read existing) | Framebuffer base in DRAM (bit 31 masked out) |
| `REG_LCD_WIN0_STRIDE` | `0x38900028` | `240` | Stride in pixels (`stride & 0xfff`) |
| `REG_LCD_WIN0_SRC` | `0x3890002c` | `0` | Source offset `(sx << 16) \| sy` |
| `REG_LCD_WIN0_SIZE` | `0x38900030` | `(240 << 16) \| 432` (`0x00f001b0`) | Window size `(w << 16) \| h` |
| `REG_LCD_WIN0_DST` | `0x38900034` | `0` | Destination position on panel `(dx << 16) \| dy` |

**Where found:**
- `emulator/machine/hw/arm/nano7_lcd.c`: Lines 6-16 (reverse engineering header comment describing register map per window w), lines 114-162 (`lcd_window()` scanout logic), and lines 196-225 (`lcd_scanout()`).
- `docs/research/direct-display-path.md`: Documents how the RetailOS commit routine `0x08080b70` programs `0x38900024` and `0x38900004 |= 0x10`.
- `docs/research/dualboot-chainloader-plan.md`: Section 4 (Phase 3.3).

**Color representation:**
- Pixel format 7 (ARGB8888) is evaluated in `nano7_lcd.c` (`lcd_pixel()`) as little-endian 32-bit words where byte 0 = Blue, byte 1 = Green, byte 2 = Red, byte 3 = Alpha.
- Bright green `0x0000FF00` (Red=0, Green=255, Blue=0) generates `0x00` in Red/Blue and `0xFF` in Green when composited into panel GRAM and written to PPM screen dumps (`lcd_dump()`).

---

### B. Home Button Polling via GPIO (Base: `0x3cf00000`)

| Register | Offset | Bit / Mask | Description |
|---|---|---|---|
| `GPIO_BANK10_DAT` | `0x3cf00144` | Bit 6 (`1 << 6`, mask `0x40`) | GPIO 0x56 level: PMU event/interrupt line (active-low) |

**Hardware Signal Flow:**
- On the physical iPod nano 7G, buttons are managed by the PMIC / PMU on I2C bus 1 (`0x3c900000`, 7-bit slave address `0x73`).
- Pressing the Home button asserts bit 0 of PMU Event Register 3 and clears bit 4 of PMU Level Register 7 (`nano7_pmu.c`).
- The PMU asserts its open-drain event interrupt line wired to SoC GPIO `0x56`.
- GPIO `0x56` maps to GPIO Bank 10, pin 6:
  $$\text{Bank } b = \lfloor 0x56 / 8 \rfloor = 10 \implies \text{Offset } 0x3cf00000 + 10 \times 0x20 + 4 = 0x3cf00144$$
  $$\text{Pin } p = 0x56 \pmod 8 = 6 \implies \text{Bit } 6$$
- Polarity is active-low:
  - Idle (unpressed): Bit 6 reads `1`
  - Pressed: Bit 6 reads `0`

**Where found:**
- `emulator/machine/hw/arm/nano7_gpio.c`: Lines 16-20 and line 197 show that input levels for GPIO $n$ read at `0x3cf00000 + b * 0x20 + 4`, bit $p$. Pin 0x56 is modeled at `0x3cf00144` bit 6.
- `emulator/machine/hw/arm/nano7_pmu.c`: Line 13 and lines 102-107 (`pmu_line()`) demonstrate that when `regs[3]` has any event bit set (e.g. Home button), `NANO7_GPIO_PMU_IRQ` (0x56) is driven to level 0.
- `docs/research/dualboot-chainloader-plan.md`: Section 3.B (Tier 1) lists hardware button registers.
- `docs/research/touch-input.md`: Section on Home and Power buttons.

---

### C. System Reset & Watchdog (Bases: `0x3c800000` & `0x3c500000`)

| Register | Address | Value | Action |
|---|---|---|---|
| `REG_WATCHDOG` | `0x3c800000` | `0x00104a00` | Watchdog timer arm / reload |
| `REG_PMGR_RESET` | `0x3c500050` | `0x000000a5` | System reset unlock / warm reset trigger |
| `REG_WATCHDOG` | `0x3c800000` | `0x000000a5` | Watchdog unlock token |
| `LR` Return | `0x22008100` | Return via `bx lr` | Chainloader handoff warm reset trap stub |

**Where found:**
- `dumps/snap6/ram-full.bin`: Disassembly of the RetailOS shutdown/reset sequence at `0x08417c9c`-`0x08417cb6`:
  ```arm
  8417c9c: ldr  r0, =0x00104a00
  8417c9e: mov  r1, #0x3c800000
  8417ca2: str  r0, [r1]           ; configure watchdog
  8417ca4: bl   0x8003258
  8417ca8: ldr  r1, =0x3c500000
  8417caa: movs r0, #0xa5
  8417cac: str  r0, [r1, #0x50]    ; write 0xa5 to PMGR/CLKCON reset
  8417cae: nop
  8417cb6: b    0x8417cae
  ```
- `dumps/bootrom-vrom.bin`: BootROM initialization at `0x200000e4`-`0x200000ec` unlocks/resets watchdog with `0xa5`.
- `docs/research/sleep.md`: Line 7 ("Shutdown unlock: Write 0xa5 to [CLKCON + 0x50] (0x3c500050) after configuring 0x3c800000=0x00104a00").
- `docs/research/bootrom.md`: Table of hardware MMIO blocks (Line 52: Watchdog Timer `0x3c800000`).
- `docs/research/dualboot-chainloader-plan.md`: Section 4 (Phase 6.3: LR convention `0x22008100` for warm reset return).

---

## 3. Memory Layout (`payload.ld`)

```
0x08000000 +----------------------------------+
           | .text.entry (_start)             |
           | .text (payload_main, loops)      |
           | .rodata                          |
           | .data                            |
0x08000130 +----------------------------------+
           | .bss (zero-filled by _start)     |
0x08000130 +----------------------------------+
           | .stack (16 KiB DRAM stack)       |
0x08004130 +----------------------------------+ <--- _stack_top
           | ... unused DRAM ...              |
0x08200000 +----------------------------------+ <--- DRAM_FALLBACK_FB
           | Framebuffer (240x432x4 = 405 KiB)|
0x08265400 +----------------------------------+
           | ... remaining 62 MiB DRAM ...    |
0x0c000000 +----------------------------------+
```

- Raw binary output: `payload.bin` (contains only `.text` and `.data`, 304 bytes total).
- Stack and BSS are marked `NOLOAD` and do not bloat the binary file.
- The stack grows downwards from `_stack_top` (`0x08004130`), safely below the fallback framebuffer at `0x08200000`.

---

## 4. Observations & Design Decisions

1. **GPIO 0x29 vs 0x3cf00144 notation in plan doc:**
   In `docs/research/dualboot-chainloader-plan.md` (Line 95), the text mentions:
   `Volume Down button: GPIO 0x29 (0x3cf00144 / VIC IRQ 32)`
   Cross-referencing `emulator/machine/hw/arm/nano7_gpio.c` showed that `0x3cf00144` is actually GPIO Bank 10 ($10 \times 0x20 + 4 = 0x144$), which holds GPIO 0x56 (the PMU event line). GPIO 0x29 is in Bank 5 ($5 \times 0x20 + 4 = 0x0a4$).
   Because NanoEmu routes the Home button event through PMU `regs[3]` to pull GPIO 0x56 low, checking `0x3cf00144` bit 6 is the exact hardware-accurate way to detect the Home button.
2. **Dynamic vs Static Framebuffer Address:**
   If Window 0 base register `0x38900024` contains a valid DRAM address when the payload takes control (e.g., `0x0bf29380` from the prior OS frame), the payload reuses and updates that existing buffer. If `0x38900024` is unconfigured (0 or outside DRAM), it programs `0x08200000`. In either case, the entire 240x432 framebuffer is filled with `0x0000FF00`.
3. **No Libc / No SDK Dependency:**
   All types (`uint32_t`, `uint16_t`, `uint8_t`) and MMIO pointer macros are defined locally without standard library headers or OS stubs.

---

## 5. Build Verification

Build command:
```bash
make -C probe/chainloader
```

Artifacts produced:
- `build/payload.elf`: 304 bytes text, 0 bytes data, 16 KiB BSS/stack
- `build/payload.bin` / `payload.bin`: 304 bytes flat raw binary ready for chainloader staging
