# S5L8740/S5L8747 Hardware Architecture Documentation

This directory contains hardware reference documentation and reverse-engineered register specifications for the Samsung S5L8740/S5L8747 application processor. Each document details peripheral register maps, physical memory layouts, interrupt routing, and hardware protocols verified from ROM and firmware analysis.

## Peripheral and Subsystem Specifications

| Document | Description |
|---|---|
| [bootrom.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/bootrom.md) | Mask ROM boot flow, ARM exception vectors, USB DWC2 DFU interface, and RSA/AES/SHA crypto verification. |
| [lcd.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/lcd.md) | LCD controller registers at `0x38900000`, 240x432 BGRA8888 framebuffer layout, panel transport, and backlight control. |
| [touch-input.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/touch-input.md) | Capacitive touch controller SSP2 serial protocol at `0x3d200000`, packet framing, coordinate mapping, and GPIO buttons. |
| [nand-fmc.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/nand-fmc.md) | Flexible Memory Controller registers at `0x38a00000`, bus phases, and PMGR hardware chip identification gating. |
| [nand-ppn.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/nand-ppn.md) | Apple Pure-Path NAND (PPN) protocol, detection pipeline, parameter page structures, and shutdown reason codes. |
| [bluetooth-stack.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/bluetooth-stack.md) | Broadcom BCM2076B1 controller interface, high-speed UART at `0x3db00000`, HCI packet framing, and OS tasks. |
| [gpu-mbx.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/gpu-mbx.md) | PowerVR MBX Lite 2D/3D GPU registers at `0x3b000000`, command FIFOs, MMU page tables, and compositor pipeline. |
| [audio-path.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/audio-path.md) | Multichannel I2S audio controllers at `0x3ca00000`, dual-engine DMA routing, and SPI codec control protocol. |
| [h264-decoder.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/h264-decoder.md) | Apple M2 multi-pipe H.264 video decoder sub-blocks (`0x39600000` - `0x39670000`), doorbell dispatch, and IRQ 45 routing. |
| [h264-deep.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/h264-deep.md) | Advanced H.264 hardware decoder register layout (`0x38f00000`), IRQ 35 routing, and host-assisted decode pipeline. |
| [itunes-sync.md](file:///home/pavliukov/ipodwork/NanoRework/docs/research/itunes-sync.md) | iTunes library database formats (`iTunesCDB`, SQLite `.itdb`), HashAB signature generation, and track synchronization workflow. |
