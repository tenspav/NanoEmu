# Apple Pure-Path NAND (PPN) Protocol and Driver Reference

The Apple Pure-Path NAND (PPN) subsystem bypasses standard ONFI commands in favor of an Apple-proprietary command set and geometry parameter structure. The driver initializes through the Flexible Memory Controller (FMC) at `0x38a00000`, probes attached chip selects, validates PPN firmware revisions, and transitions into Virtual Flash Layer (VFL) operation.

## Initialization and Detection Pipeline

NAND initialization starts at `0x80fb6e0` and invokes the device detection procedure `0x84cce2c`.

```
+-------------------------------------------------------------------------+
| NAND Init Entry (0x80fb6e0)                                             |
+-------------------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------------------+
| Detection State Machine (0x84cce2c)                                     |
|  1. Reset & READID (0x812a760, 0x84d1264): Probes CE0-7 (Cmd 0x90, 0x00)|
|  2. Signature Check (0x84cd280): Matches "PPN" and Version 0x01xxxx     |
|  3. Controller Version (0x84cd148): Reads 16 ASCII bytes (Cmd 0xEE)     |
|  4. Manufacturer ID (0x84cd1d4): Reads 8 bytes (Cmd 0x90, Addr 0x30)    |
|  5. Parameter Page (0x812a214): Reads 512 bytes (Cmd 0x92, 0x00)        |
|  6. Geometry Bounds Check (0x812ad5c): Validates CAU and block ranges   |
+-------------------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------------------+
| FMC Timing Setup & SET FEATURES                                         |
|  - FMC timing registers programmed from parameters (0x84cd090)         |
|  - SET FEATURES executed per active CE (0x812abc0 -> 0x812aa5c)        |
|  - Transition to VFL/FTL runtime storage layer                          |
+-------------------------------------------------------------------------+
```

### Detection Steps

1. Bus Reset and CE Probing (`0x812a760`, `0x84d1264`):
   - Sends reset command `0xFF` to CE0 through CE7.
   - Issues `READID` (command `0x90`, address `0x00`, 8 bytes) to each chip select.
   - Any chip select returning all `0x00` or all `0xFF` is skipped.
   - Each chip select whose first 6 bytes match CE0 increments the active CE count.
   - Total CE count is stored at `0x08d0d3e0`. Count must be 1 or 2 (0 raises `"No CE detected!"`; > 2 triggers failure).
2. Protocol Signature and Version Validation (`0x084cd280`):
   - Bytes `id[0..2]` must match ASCII `"PPN"` (`0x50 0x50 0x4E`).
   - Version word `(id[3] << 16) | (id[4] << 8) | id[5]` must fall between `0x010000` and `0x01FFFF` (PPN version 1.x.x). Stored at `0x08d0d434`.
   - Versions below `0x010500` require multiple chip enables; a single CE triggers `"PPN 1.0 parts should have multiple CEs"`. Versions `>= 0x010500` execute an alternate die count check at `0x84ccf0c`.
3. Controller Version Read (`0x084cd148`):
   - Sequence: Command `0xEE`, address `0x80, 0x90`, sub-commands `0xE7, 0x77, 0x7D`, status wait, command `0x7A`.
   - Reads 16 ASCII bytes (`"PPN Controller Version: %16.16s"`). The string must be identical across all detected chip selects.
4. Manufacturer ID Read (`0x084cd1d4`):
   - Sequence: `READID` command `0x90`, address `0x30`, 8 bytes.
   - Returned data must be identical across all chip selects.
5. Device Parameter Page Read (`0x0812a214`):
   - Sequence: Command `0x92`, address `0x00`, sub-commands `0x97, 0x77, 0x7D`, status wait, command `0x7A`.
   - Reads 512 bytes of packed device geometry parameters.

## Parameter Page Geometry Limits (`0x0812ad5c`)

The 512-byte parameter page is strictly validated against the following hardware bounds:

| Offset | Parameter | Accepted Range | Description |
|---|---|---|---|
| `+0x10` | caus_per_channel | 1 to 64 | Number of Contiguous Addressable Units per channel |
| `+0x14` | cau_bits | 1 to 7 | Address bit width for CAU indexing |
| `+0x18` | blocks_per_cau | `0x0400` to `0x2134` | Physical blocks contained in one CAU |
| `+0x1c` | block_bits | 10 to 15 | Address bit width for block indexing |
| `+0x20` | pages_per_block | 64 to 512 | Number of physical pages per block |
| `+0x24` | pages_per_block_slc | 64 to 256 | Number of SLC-mode pages per block |
| `+0x28` | page_address_bits | 5 to 9 | Bit width of page address within block |
| `+0x2c` | channels | 1 to 2 | Number of independent flash channels |
| `+0x30` | bus_width | 1 to 4 | Bus width and interleave factor |
| `+0x34` | page_size | >= `0x2020` | Page data size in bytes (minimum 8224 bytes) |
| `+0x38` to `+0xf0` | Timing parameters | 1 to 255 | Bus timings (`tRC`, `tWP`, etc.); all must be non-zero |

## Minimal Emulation Identity Profile

The simplest valid hardware configuration is a 2-CE setup using PPN version 1.0.0, avoiding the extended die-count check:

- CE0 and CE1 `READID` (address `0x00`): `50 50 4E 01 00 00 00 00` (`"PPN"` + version 1.0.0). CE2 through CE7 return all `0xFF`.
- CE0 and CE1 Manufacturer ID (address `0x30`): Identical 8 bytes (e.g., `45 00 00 00 00 00 00 00` for SanDisk).
- Controller Version (command `0xEE` / `0x7A`): Identical 16 ASCII bytes on both chip selects.
- Parameter Page (command `0x92` / `0x7A`): 512 bytes populating the validated fields above.

## Post-Detection Setup and VFL Transition

On successful detection:
1. `0x84cd090` calculates FMC bus timing words from the parameter page and writes them to `[0x0896e8e0 + 0x24]` and `[0x0896e8e0 + 0x28]`. The word at `+0x24` is OR-ed into FMC control register `+0x00` during every chip select assertion.
2. `0x812abc0` executes `SET FEATURES` via `0x812aa5c`:
   - Command `0xEF`, address `0x90, 0x00`, 4 data bytes `{01, 00, 00, 00}`, sub-commands `0xE7, 0x77, 0x7D`.
3. Memory buffers are allocated and `0x80fb6e0` returns 0. If initialization fails, wrapper `0x80da050` returns `0x80000001`.
4. Subsequent operations initiate Virtual Flash Layer (VFL) and Flash Translation Layer (FTL) page transfers using FMC command `0x000100e4` with commands `0x00` and `0x30`.

## System Shutdown Reason Codes

Any abort condition invokes `0x840b86c(reason)` prior to system shutdown:

| Reason Code | Caller | Cause |
|---|---|---|
| 3 | `0x08000d10` | Chip ID check failed in PMGR block (`0x3d100000`) |
| 4 | `0x08255080`, `0x0825508e` | Filesystem lookup for `"iPod_Control\Device"` (`0x080df498`) failed and `0x83fbf40(0) != 18` |
| 5 | `0x0819a186` | Hardware status assertion (`[r4 + 0x84]`) |
| Unknown | `0x081ca62e` | Timeout reached after ten 1-second retry cycles |
