I finished the analysis, but it shows the premise is reversed: NAND identification isn't what's failing. The reset loop you saw runs *inside* the shutdown routine, and what triggers the shutdown is a chip-ID check on the PMGR block (0x3d100000).

**d) Failing check**
- The shutdown routine at 0x08417c9c writes 0x00104a00 to 0x3c800000, then calls 0x08003258 → 0x080057d0, which ends in the NAND reset at 0x08010b4c. After that it writes 0xa5 to 0x3c500050 (0x08417cac) and spins at 0x08417cae. So the reset loop is the controller being shut down, and step 3 of your trace is that same 0xa5 write.
- The caller is the boot path at 0x08000cdc: `bl 0x080010d4; cbz r0 → 0x08000d0a`, then `0x0840b86c(3)` (stores a reason string at 0x2202fe00), then `bl 0x08417c9c` at 0x08000d10.
- The check at 0x080010d4 calls 0x08000e6c, which reads PMGR+0x00 at pc 0x08000e84:
  - If bit0 is 0, chip-id is set to 1. That fails the test at 0x080010e4, which wants `chipid == 0x8740`.
  - If bit0 is 1, it reads +0x04 and +0x08, and chip-id = `(+0x08) >> 16`.
  - The second operand is a static value, 0x00250015 at 0x0890bb70. It passes the `>>16 == 0x25` and `&0xffff >= 3` tests.
- Your w_0x4.log shows only the +0x00 read and none of +0x04/+0x08. So bit0 was 0 in that run.
- Current `machine/hw/arm/nano7.c` already presets PMGR+0x00 = 1 and +0x08 = 0x87400000. That change and the current binary (08:18) are newer than the log (04:18), so this failure may already be fixed. My one run of the current build got a different result, trapping in the IRAM vectors around 0x22000004, probably because my options didn't match yours. Please re-run with your usual command to check.

**b) FMC register map (0x38a00000)**
| Offset | Use |
|---|---|
| +0x00 | Chip-select/config: `base | ((1<<cs)<<1)`; 1 = deselect/idle |
| +0x04 | Control/"go": 1 = send address bytes; 0x000100e4 = data transfer (with +0x30 = len−1, +0x14 = 0x100); 0xea = start status polling; 0x20 = stop polling |
| +0x08 | Command byte |
| +0x0c, +0x10 | Address bytes, packed 4 per word, LSB first |
| +0x2c | Number of address bytes − 1 |
| +0x48 | Interrupt/status, write-1-to-clear (bits below) |
| +0x4c | Status byte captured by polling |

Bits in +0x48, all waited on by 0x081810b8 with `tst status, mask`, a 20000-tick timeout, then cleared by writing the mask back:
- bit1 (0x2): command phase done
- bit2 (0x4): address phase done
- bit3 (0x8): data phase done
- bit23 (0x800000): polled status ready, used by 0x0840c3e8
- bit24 (0x1000000): cleared afterwards

Returning all-ones on +0x48 satisfies every one of these waits.

**a) Identification flow**
This is Apple's PPN NAND driver (strings "Apple PPN NAND Driver", "No PPN Device Detected", "Unable to Read ChipID from all CEs!" at 0x0812a824).
- Detection loops over chip selects at 0x084ce38e–0x084ce406 and calls 0x0812aa5c(cs, 0x90, buf, 4).
- That helper (0x0812aa5c) is not a classic READID. It selects the chip via +0x00, then sends command 0xEF, 2 address bytes (0x90, 0x00) and 4 data bytes. It then sends commands 0xE7, 0x77, 0x7D, polls status (+0x4c) and deselects.
- The low-level steps are cmd 0x0840b1fc, address 0x0840944c, data 0x0810d3a8 and status poll 0x0840c3e8.

**c) Accepted IDs:** I didn't find this. There are no compares against 0x2c/0xad/0xec/0x98/0x45 in the driver (the one 0x45 hit is jump-table data). Acceptance looks like a PPN signature or firmware-version check (strings "Unsupported PPN FW Ver", "PPN Controller Version: %16.16s"), and I didn't trace its exact bytes. I'd only chase this if the PMGR fix gets boot past 0x08000d10.

I didn't modify anything in the repo. My disassembly and the test-run log are in `/tmp/fmcanal/`.
