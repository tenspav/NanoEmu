# S5L8740/S5L8747 Flexible Memory Controller (FMC) Reference

The Flexible Memory Controller (FMC) is mapped at MMIO base `0x38a00000` and interfaces parallel NAND flash and Apple Pure-Path NAND (PPN) devices. Flash bus operation requires passing an earlier Power Manager (PMGR) hardware identification check during initial boot.

## Boot Validation and PMGR Chip Gating

Before NAND initialization, the boot sequence at `0x08000cdc` validates SoC identity via `0x080010d4`.

### PMGR Identification Registers (`0x3d100000`)
Routine `0x08000e6c` queries the PMGR block:
- Offset `+0x00`: Bit 0 indicates valid hardware identification strapping. If bit 0 is 0, the routine sets the chip ID to 1, causing validation failure at `0x080010e4`.
- Offset `+0x04`: Secondary identification register.
- Offset `+0x08`: Primary chip identification register. The value right-shifted by 16 (`(+0x08) >> 16`) must equal `0x8740`.
- Static Configuration: A table entry at `0x0890bb70` contains `0x00250015`. The code requires `val >> 16 == 0x25` and `(val & 0xffff) >= 3`.

### Failure Shutdown Sequence
If the check fails, the bootloader calls `0x0840b86c(3)`, records reason code 3 at memory location `0x2202fe00`, and invokes `0x08417c9c`:
1. Writes `0x00104a00` to the watchdog timer at `0x3c800000`.
2. Calls `0x08003258` -> `0x080057d0`, triggering a NAND controller reset at `0x08010b4c`.
3. Writes `0xa5` to power register `0x3c500050` (`0x08417cac`).
4. Enters an infinite spin loop at `0x08417cae`.

## FMC Register Map (`0x38a00000`)

Offsets below are relative to FMC base `0x38a00000`.

| Offset | Name | Description |
|---|---|---|
| `+0x00` | FMC_CS_CFG | Chip select and bus configuration: `base | ((1 << cs) << 1) | 1`. Value `1` idles/deselects the bus. |
| `+0x04` | FMC_CTRL | Execution trigger and transfer control (command opcodes below) |
| `+0x08` | FMC_CMD | NAND command byte latch |
| `+0x0c` | FMC_ADDR0 | Address bytes 0 through 3 (packed 4 bytes per 32-bit word, LSB first) |
| `+0x10` | FMC_ADDR1 | Address bytes 4 through 7 |
| `+0x14` | FMC_XFER_CFG | Transfer configuration mode: `1` for arm, `0x100` for PIO data read |
| `+0x24` | FMC_TIMING | Bus timing control word (OR-ed into control on chip select) |
| `+0x28` | FMC_PIO_STATUS| PIO transfer status: low byte must match cumulative bytes read |
| `+0x2c` | FMC_ADDR_CNT | Address byte count minus 1 (`count - 1`) |
| `+0x30` | FMC_DATA_LEN | Data transfer byte count minus 1 (`length - 1`) |
| `+0x48` | FMC_INT_STATUS| Interrupt status and phase completion (write-1-to-clear) |
| `+0x4c` | FMC_POLL_DATA | Status byte captured from device during polling |
| `+0x80` | FMC_DATA_FIFO | 32-bit PIO data FIFO read port |

### Control Trigger Commands (`+0x04`)
- `0x01`: Transmit programmed address bytes (`+0x0c`/`+0x10`).
- `0x02`: Transmit programmed command byte (`+0x08`).
- `0x20`: Abort or terminate active polling/data operation.
- `0x22`: Arm hardware for incoming data transfer.
- `0xEA`: Start hardware status polling.
- `0x1E0`: Start PIO data read from `+0x80`.
- `0x000100e4`: Start direct data transfer (configured with `+0x30 = len - 1`, `+0x14 = 0x100`).

### Status Flags (`+0x48`)
The status register at `+0x48` indicates bus phase completions. Wait loops in `0x081810b8` test masks with a 20,000-tick timeout and clear flags by writing the mask back.

| Bit | Mask | Phase | Handled By |
|---|---|---|---|
| 1 | `0x00000002` | Command phase complete | `0x0840b1fc` |
| 2 | `0x00000004` | Address phase complete | `0x0840944c` |
| 3 | `0x00000008` | Data phase complete | `0x0810d3a8` |
| 20 | `0x00100000` | PIO read burst complete | Read loop termination |
| 23 | `0x00800000` | Polled status byte valid in `+0x4c` | `0x0840c3e8` |
| 24 | `0x01000000` | Status polling cycle finished | Post-poll cleanup |

Returning `0xffffffff` on reads of `+0x48` satisfies all status wait loops in minimal software models.

## Bus Operation Sequences

### Command Phase (`0x0840b1fc`)
1. Write command byte to `+0x08`.
2. Write `2` to `+0x04`.
3. Wait until `+0x48` bit 1 sets.
4. Write `0x02` to `+0x48` to acknowledge.

### Address Phase (`0x0840944c`)
1. Write packed address words to `+0x0c` and `+0x10`.
2. Write byte count minus 1 to `+0x2c`.
3. Write `1` to `+0x04`.
4. Wait until `+0x48` bit 2 sets.
5. Write `0x04` to `+0x48` to acknowledge.

### PIO Data-In Phase
1. Write transfer length minus 1 to `+0x30`.
2. Write `0x100` to `+0x14`.
3. Clear bits 8 and 26 in control register `+0x00`.
4. Write `0` to `+0x28`.
5. Write `0x1E0` to `+0x04`.
6. Read 32-bit words from FIFO register `+0x80`. After each word read, the low byte of `+0x28` must match the cumulative byte count (4, 8, 12, ...).
7. Write `0x00100000` to `+0x48` to clear bit 20.
8. Write `0x20` to `+0x04` to conclude the data phase.

### Status Polling Phase (`0x0840c3e8`)
1. Write `0xEA` to `+0x04`.
2. Wait until `+0x48` bit 23 sets.
3. Read the captured device status byte from `+0x4c`.
4. Write `0x01000000` to `+0x48` to clear bit 24.

### PPN Feature Configuration (`0x0812aa5c`)
Executed per chip select during detection loops (`0x084ce38e` - `0x084ce406`):
1. Assert target chip select on `+0x00`.
2. Transmit command `0xEF` (SET FEATURES).
3. Transmit two address bytes: `0x90`, `0x00`.
4. Transmit four configuration data bytes: `0x01, 0x00, 0x00, 0x00`.
5. Transmit sub-commands `0xE7`, `0x77`, `0x7D`.
6. Poll status byte from `+0x4c`.
7. Deselect chip select (`write +0x00 = 1`).
