# iPod nano 7G Hardware Specification

Everything the NanoEmu project has learned about the nano 7G hardware, from our own device dumps and reverse engineering. Items marked **[confirmed]** were verified against the real device or its firmware. Items marked **[unconfirmed]** come from public sources or inference and still need measurement.

## SoC

| | |
|---|---|
| Chip | Samsung S5L8747-class application processor ("N31" platform) **[confirmed]** |
| CPU | ARM Cortex-A5, single core, ARMv7-A + Thumb-2 **[confirmed]** |
| CPU clock | PLL1 locked at 684 MHz; CPU runs at 342 MHz (div 2). One-register boost to 684 MHz identified (CLKCON 0x3c500000) **[confirmed]** |
| Boot ROM | 64 KB at 0x20000000 (dumps mirror 4x), reset vector 0x200000a8, Apple SecureROM with DFU over USB **[confirmed: we dumped it]** |

## Memory

| | |
|---|---|
| DRAM | 64 MB at 0x08000000-0x0bffffff **[confirmed]** |
| IRAM (SRAM) | 256 KB at 0x22000000-0x2203ffff **[confirmed]** |
| App heap limit | ~8 MB (OS-imposed per-app limit; the reason for the full-RAM homebrew goal) **[confirmed]** |
| Unified memory | GPU shares system DRAM; no separate VRAM chip **[confirmed]** |

## Display

| | |
|---|---|
| Panel | 240x432, 32-bit (BGRA8888/ARGB), 60 Hz sync **[confirmed]** |
| LCD controller | MMIO 0x38900000, two windows/layers **[confirmed, emulated]** |
| 2D blitter | MMIO 0x39000000, copy/blend/fill, 5 blend modes **[confirmed, emulated]** |
| Backlight | via serial engine 0x3de00000 (IRQ 52) **[confirmed]** |

## GPU

| | |
|---|---|
| Model | Imagination PowerVR MBX Lite (strings "MBX1", threads MBXFlushThread/MBXFlipHandlerThread, 2D core "GKMBX2D") **[confirmed presence]** |
| MMIO | 0x39100000 / 0x39200000 / 0x39300000 + control 0x3b000000, FIFO windows **[confirmed]** |
| IRQ | 53 **[confirmed]** |
| Clock | Stock 133 MHz (PLL2/2); unlockable to 171 MHz (safe, driver-known) or 266 MHz **[confirmed]** |
| Emulation | Spec in docs/research/gpu-mbx-spec.md, not implemented yet |

## Video decode

| | |
|---|---|
| Decoder | Apple M2 H.264 multi-pipe engine ("AppleM2H264MPDDriver") **[confirmed presence]** |
| MMIO | 0x39600000-0x39670000 (sub-blocks 0x3960/61/63/64/65/66) **[confirmed]** |
| IRQ | 45 **[confirmed]** |
| Emulation | Minimal model spec in docs/research/h264-decoder.md |

## Audio

| | |
|---|---|
| Output thread | "MeCCAOutputTask" (0x080a6ec7) **[confirmed]** |
| Codec | SPI-attached (IRQ 9), 16-bit register protocol **[confirmed]** |
| Sample engine | I2S + DMA engines 0x38200000 (IRQ 16, audio: ch 2 -> I2S0 TX) / 0x38700000 (IRQ 17), linked descriptors **[confirmed, Session 27: IRAM thunks 0x22001168/0x2200116c pass engine id 1/2 to ISR 0x220010ee]** |
| Decode | MP3/AAC decode co-processor 0x38500000 (IRQ 18, ID 0x004e7309), IRAM mailbox 0x2203ff00 / PCM 0x22038100 |
| Emulation | Session 27: DMA, codec, I2S (PCM to --audio-out), decoder job/result protocol (silent frames); real MP3 decode not yet |

## Wireless

| | |
|---|---|
| Bluetooth | Classic BR/EDR + BLE: HCI, L2CAP, RFCOMM, A2DP, pairing manager with SSP auto-accept, persistent keys in btdevices.plist **[confirmed in OS]** |
| Chip interface | UART-attached (BT Uart RxLoop thread 0x081920ac) **[confirmed]** |
| Chip model | **[unconfirmed]** (Broadcom/Cypress-class, typical for Apple of that era) |
| Wi-Fi | A Broadcom BCM4330 die is present in the package but unrouted/disabled; unusable **[confirmed present, disabled]** |
| Research | docs/research/bluetooth-stack.md |

## Storage

| | |
|---|---|
| NAND flash | 16 GB total, 14.7 GiB user FAT32 partition; PPN controller protocol ("PPN" ID, 16 KB pages, 2 CEs) **[confirmed, emulated]** |
| Controller (FMC) | MMIO 0x38a00000 + FMSS command sequencer at +0xc00 (IRQ 54) **[confirmed, emulated]** |
| FTL | Apple sftl/PPNVFL layers with per-page metadata **[confirmed]** |

## Ports and peripherals

| | |
|---|---|
| USB | DWC2 OTG at 0x38400000 + PHY 0x3c400000 (DFU in bootrom, disk mode in OS) **[confirmed]** |
| Touch | Serial port 2 (0x3d200000, IRQ 11) + touch-ready GPIO 0x26, checksummed packet protocol **[confirmed, emulated]** |
| Buttons | Home/Power/Play-Pause via PMU on I2C1 (0x3c900000, slave 0x73); volume via GPIO **[confirmed, partially emulated]** |
| Interrupt controller | Dual PL192-style VIC at 0x38e00000/0x38e01000 + glue 0x38e02000 **[confirmed, emulated]** |
| Timers | 8 channels at 0x3c700000 (3 MHz source; 500 Hz OS tick on ch1; 64-bit 1 MHz free-running counter) **[confirmed, emulated]** |
| Crypto | AES engine 0x38c00000, SHA-1 0x38000000, RSA 0x3d000000 (used by bootrom DFU image auth) **[confirmed]** |
| Sensors | Accelerometer for Nike+/Fitness **[unconfirmed interface]** |

## Clocks and power

| | |
|---|---|
| Clock control | CLKCON at 0x3c500000 (PLL + per-block gating), PMGR at 0x3d100000 (chip ID 0x8740, power gates) **[confirmed]** |
| Power management | PMU on I2C slave 0x73: battery voltage, temperature, charger status, RTC, buttons **[confirmed, emulated]** |
| Watchdog | 0x39700000 **[confirmed]** |

*Last updated: v0.5.0 (2026-10-14). Sources: our own device dumps (see probe/) and the research notes in docs/research/.*
