# S5L8747 Deep Sleep, Power States & Wake Architecture (iPod nano 7G)
1. Power State Controller: Block 0x39700000 (NOT PMGR 0x3d100000, which is ChipID 0x8740 @ +0x08).
   - State enter/exit registers: 0x3970000c (request enter 1<<mode), 0x39700010 (request exit), 0x39700014 (status/busy), 0x39700018 (exit config), 0x3970001c (mode parameter), 0x39700100 (state mask).
   - 6 hardware power modes (table 0x0890bb80) mapped from 16 OS states (table 0x08753968): Mode 1 (0x001d0098), Mode 2 (0x002900c8), Mode 3 (0x00330080), Mode 4 (0x00110177), Mode 5 (0x001d00e2), Mode 6 (0x00140065).
2. Clock Gating & Shutdown (CLKCON 0x3c500000):
   - Peripheral gates gated at 0x3c500048, 0x3c50004c, 0x3c500058, 0x3c500068, 0x3c50006c (routine 0x08001db4).
   - Shutdown unlock: Write 0xa5 to [CLKCON + 0x50] (0x3c500050) after configuring 0x3c800000=0x00104a00 (loop 0x08417cae).
3. DRAM Retention & Execution in IRAM (0x22000000):
   - DRAM controller (DMC 0x3d700000) puts 64MB DRAM into self-refresh: [0x3d700100] |= 0x0a000000, [0x3d70011c]=1, [0x3d700120]=1, polls [0x3d700104] bit 22/18 (0x440000).
   - CPU executes suspend routine from 256KB IRAM (0x220014c2, 0x22001560) using DSB + WFI (0x0862ade8/0x22000f64); IRAM is fully retained.
   - S3 Resume / Hibernate: Checked by bootloader at 0x08ddfce8 using 'HibernateHeader' (magic 'hibe' 0x65626968 at +0x00, ver 1 at +0x04, entry point at +0x10 -> "S3_RESUME - jumping to app" 0x08ddfd1c).
4. Wake Sources & Interrupt Wiring:
   - PMU Event Line: GPIO 0x56 (0x3cf00144 bit 6, active-low, GPIO group 2 -> VIC IRQ 31, callback 0x080fb268). PMU (I2C1 0x3c900000, slave 0x73) sources:
     * Sleep/Wake (Power) button: PMU reg 3 bit 1, wake status reg 0x7e bit 1 ("General.SleepWakeButtonEnabled" 0x087fb760).
     * Home button: PMU reg 3 bit 0, wake status reg 0x7e bit 0. Play/Pause button: PMU reg 3 bit 3.
     * USB VBUS / Charger connect: PMU reg 0x05 bit 6 (ext power) & bit 3 (charger good).
     * RTC Alarm: PMU regs 0x45..0x49 (matches 1Hz counter 0x40..0x43). Time prefs: "_minutes_to_standby" (0x0880878e), "_minutes_to_hibernate" (0x08807d99).
   - Direct GPIO Wake: Volume Up (GPIO 0x28) / Down (GPIO 0x29) and Touch (GPIO 0x26), routed via GPIO group 1 -> VIC IRQ 32.
