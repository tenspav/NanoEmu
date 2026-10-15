# iPod nano 7G (S5L8747/S5L8740) E-FUSE / GID Hardware Interface
1. E-Fuse / Chip ID Controller Base: 0x3d100000 (PMGR / SysCon block)
- 0x3d100000: Chip ID enable / status (bit 0 checked by kernel at 0x08000e84)
- 0x3d100004: Chip revision / strapping (rev [27:24], boot bypass bit 18 checked at BootROM 0x200000f8)
- 0x3d100008: Hardware Chip ID register (bits [31:16] = 0x8740 S5L8740, checked at 0x08000e98)
- 0x3d10000c: E-Fuse Reg 0 / ECID low slice (read & unpacked by kernel at 0x08130a1a/2a/4c)
- 0x3d100010: E-Fuse Reg 1 / ECID high slice (read & unpacked by kernel at 0x08130a3a/50)
- 0x39700104: Security fuse register (bits [1:0] != 0 = production / secure boot, BootROM 0x20002254)
2. GID / UID Key Security Architecture:
- AES Engine MMIO 0x38c00000: Slot select at 0x38c0006c, bitwise-NOT lock register at 0x38c00088.
- Slot 0 = Software key (0x38c0004c-0x68); Slot 1 = GID key; Slot 2 = UID key.
- Direct read: IMPOSSIBLE. BootROM 0x20001eb0 skips key copy if slot != 0; keys route via internal bus.
3. Software Chip ID, ECID & Serial:
- BootROM: Checks image magic "8740" (0x2000079c); exports static DFU S/N "87400000000001" (0x2000bdde).
- Kernel Chip ID: 0x08000e6c reads 0x3d100000-08, caches {id: 0x8740, rev: 0x0001} at DRAM 0x08ae2170.
- Kernel ECID: 0x08130a0c unpacks 64-bit ECID from 0x3d10000c/10 into (r0, r1); formatted by 0x081d9a24.
- Device Serials: S/N "DCYJR0JFF0GT" (0x08a8d650), Board MLBN "C07247333L9F" (0x08a8d750).
4. IRAM Boot-Args (0x22025000):
- Magic "8740" (0x30343738), version "2.0" (0x03302e32), SHA-1 digest at 0x22025040 (mirrored at DRAM 0x093526a4).
