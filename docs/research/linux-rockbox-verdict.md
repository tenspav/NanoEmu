# iPod nano 7G: iPodLinux, Rockbox & Linux Kernel Feasibility Report
1. Existing Work & Prior Art:
   - iPodLinux (legacy): PortalPlayer PP5002-PP5022 only (iPod 1G-5.5G, Mini, Nano 1G) via uClinux 2.4/2.6; defunct, zero S5L support (http://www.ipodlinux.org).
   - emCORE (freemyipod): Bootloader for Samsung ARM9 SoCs (iPod Classic 6G/S5L8702, Nano 2G/S5L8701; partial 3G/4G); deprecated, no Nano 5G-7G support (https://freemyipod.org/wiki/EmCORE).
   - Rockbox: Stable official targets for Classic 6G/7G (S5L8702) and Nano 2G (S5L8700/8701); Nano 3G/4G bootloader runs from RAM only (no storage driver); Nano 5G-7G completely unported (https://www.rockbox.org, https://freemyipod.org/wiki/Status).
   - Public S5L8740/S5L8747 Work: FreeMyIPod (q3k) developed bootrom exploits (S5Late, wInd3x) and a Linux 6.2 fork (apple_n31_defconfig, U-Boot) booting Nano 7G to LCD framebuffer and USB CDC ethernet gadget over DFU (https://freemyipod.org/wiki/Linux, https://github.com/freemyipod/linux). S5L8747 also powers Apple Lightning video dongles running Darwin.
2. Rockbox S5L87xx Drivers vs. S5L8747 Hardware Delta:
   - Core & Clocks: S5L8700/8702 are ARM926EJ-S (ARMv5TEJ, ~200MHz, legacy VIC/timers) vs. S5L8747 Cortex-A5 (ARMv7-A, 342/684MHz, VFPv4/NEON, dual PL192 VIC, MMU, complex CLKCON 0x3c500000).
   - Peripherals: S5L8702 uses PATA/IDE (Classic) or raw NAND (Nano 2G) vs. Nano 7G FMC (0x38a00000) + Apple PPN FTL; Clickwheel GPIO vs. capacitive multi-touch serial (TI 343S0538 at 0x3d200000); 1-layer LCD vs. 2-layer LCD (0x38900000) + 2D blitter + MBX Lite GPU; CS42L55 audio vs. Cirrus CLI1599A1 + DMA + MP3 DSP (0x38500000).
   - Overlap & Value: Only Synopsys DWC2 USB OTG (0x38400000) shares architecture. Reusability for NanoEmu or an S5L8747 port is nil; NanoRework's RE (docs/HARDWARE.md) is far more accurate and advanced.
3. Feasibility Ladder for Nano 7G:
   - [C] Borrowing Rockbox Drivers for Emulator: Useless. NanoEmu already models 8747-specific FMC/PPN, 2D blitter, LCD, Touch, PL192, and DMA. Rockbox's ARMv5 drivers don't match 8747 hardware.
   - [B] Minimal Linux Kernel Port: Highest Feasibility (Proven by FreeMyIPod). Linux 6.2 boots on N31. Mainline needs: (a) DTS for Cortex-A5 + PL192 + timers (0x3c700000) + simplefb/DRM (0x38900000), (b) serio/tty touch driver, (c) ALSA I2S DMA driver, (d) MTD/FTL driver for FMC PPN or ramdisk rootfs.
   - [A] Rockbox Port (New Target): Moderate Feasibility, Poor Fit. Requires backporting Rockbox to Cortex-A5 MMU, writing a touch UI from scratch, and reverse-engineering a full Apple PPN FTL inside Rockbox.
4. Rockbox vs. Custom OS / Bare-Metal for User's Goals (Games, Music, Homebrew):
   - Rockbox: Pros: mature audio codecs, music DB. Cons: no GPU/blitter acceleration, clunky touch UI, no BT Classic HID gamepad stack, massive PPN FTL hurdle.
   - Bare-Metal Homebrew (e.g. ViceNano chainloader): Best for Games. Full access to 64MB DRAM, zero OS overhead, direct blitter/MBX control, chainloadable from RetailOS with zero brick risk.
   - Embedded Linux (FreeMyIPod): Best for Ecosystem. Native BlueZ for BT gamepads, SDL2, standard ALSA, and multi-tasking, sacrificing ~20MB RAM to the kernel.
   - Verdict: Skip Rockbox. Use Bare-Metal chainloader for ViceNano/high-performance games; use/extend FreeMyIPod Linux for generic homebrew and Bluetooth gamepad support.
