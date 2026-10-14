Hardware ECC: FMC sub-block at 0x38a00800 (FMC base 0x38a00000; FMSS sequencer at +0xc00).
Registers:
- +0x804: ECC Command: ((len & 0x7ff) << 16) | cmd (0x1e1=Decode at 0x084cbd14, 0x1e2=Encode at 0x084cbe18; 0x040001e1 for 1KB).
- +0x808: Control (0x02000000). +0x81c: Reset/Arm (1). +0x820: Config (0x80000000).
- +0x80c: ECC Result (init 1): bit 0 = uncorrectable (0x80000002), bit 1 = corrected (1), clean (0).
- +0x810: ECC Status: init 0x3f1f73af; poll bit 0 for done (timeout 20k ticks); write 1 to ACK/clear.
- +0x818: ECC History (init 0x04000400): read after completion for syndrome/bitflip stats.
Driver Strings & Routines:
- 0x084cbdc0: "timed out waiting for ECC operation to complete"; 0x084cbdf4: "ECC result 0x%08x history 0x%08x\n".
- 0x084ed764: "reading %d:0x%08x to %p\n"; 0x084ed824: "timed out waiting for parity transfer to complete %d:0x%08x/%d\n".
- 0x084eda2c: "encode failed %d:0x%08x/%d\n"; 0x08784afc: "error reading pages from 2nd boot loader area\n".
- PPN runtime: 0x08547ea8 "BTOC stomping lba:%d to S_TOK_UECC", 0x0854baf4 "sftl: uECC flag in meta; lba:0x%llx, meta:0x%x".
ECC Scheme & Spare Usage (Raw Mode, 0x084ed600 read / 0x084ed8d4 write):
- Algorithm: 32-bit BCH ECC per 1024-byte sector (GF(2^13), 32 * 13 = 416 bits = 52 bytes / 0x34 parity).
- 2nd Bootloader Page Layout: 3 sectors of 1024 B data (3072 B total, 0xc00) interleaved with 52 B parity (3228 B raw).
What Raw-NAND Dumper Must Know:
- Boot/2nd loader area uses raw FMC 32-bit BCH (interleaved 52B parity + 1024B data per sector; decode via +0x800).
- Main OS storage uses PPN (Pure-Page NAND): 16KB pages (4x4KB sectors); ECC is on-die, meta byte 1 bit 1 = uECC.
