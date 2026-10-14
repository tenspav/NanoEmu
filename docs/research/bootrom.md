# S5L8740/S5L8747 Boot ROM Reference

The S5L8740/S5L8747 boot ROM is a 64 KB mask ROM mapped at `0x20000000`. It initializes fundamental SoC hardware, exposes a Synopsys DWC2 USB Device Firmware Upgrade (DFU) interface, and verifies RSA-signed boot images before branching to payload code.

## Memory Map and Vectors

The boot ROM executes from `0x20000000` (first 65,536 bytes of physical ROM). The ARM exception vector table is mapped at the base.

| Address | Vector | Destination | Description |
|---|---|---|---|
| `0x20000000` | Reset | `0x200000a8` | Hardware reset entry point (`b 0x200000a8`) |
| `0x20000004` | Undefined Instruction | `0x200003fc` | Loaded from literal at `0x20000090` |
| `0x20000008` | Software Interrupt (SWI) | `0x2000040c` | Loaded from literal at `0x20000094` |
| `0x2000000c` | Prefetch Abort | `0x20000424` | Loaded from literal at `0x20000098` |
| `0x20000010` | Data Abort | `0x20000438` | Loaded from literal at `0x2000009c` |
| `0x20000014` | Reserved | `0x20000014` | `nop` |
| `0x20000018` | IRQ | `0x20000450` | Loaded from literal at `0x200000a0` |
| `0x2000001c` | FIQ | `0x20000460` | Loaded from literal at `0x200000a4` |

Driver dispatch table is located at `0x20000020`.

### Boot State Structure

The global bootrom state pointer is located at `0x2202fff8`, pointing to `0x2202ba3c`.

| Offset | Type | Description |
|---|---|---|
| `+0x24` | Pointer | Driver vtable pointer (`0x20000020`) |
| `+0x28` | uint8 | Busy flag |
| `+0x29` | uint8 | Boot-ready state: `1` after image accepted, `2` after transfer drains |
| `+0x47` | uint8 | USB-DFU active flag (cleared at `0x20004488`) |
| `+0xa0` | uint32 | Detection state machine index |
| `[state+0x630]+0x47` | uint8 | Main-loop DFU exit condition flag |

When `state+0x29` reaches `2`, the bootrom transfers execution to the loaded payload via `[[state+0x3c]+0x20]`.

## Hardware MMIO Blocks

The bootrom accesses hardware blocks across the peripheral address space during initialization and signature verification.

| Peripheral | MMIO Base | Function | Code References |
|---|---|---|---|
| SHA-1 Engine | `0x38000000` | Hardware SHA-1 hashing accelerator | `0x200030ec` |
| USB 2.0 OTG (DWC2) | `0x38400000` | USB device controller core | `0x2000a1ec`, `0x2000a878` |
| AES Crypto Engine | `0x38c00000` | Hardware AES symmetric cipher engine | `0x200022f8` |
| VIC0 | `0x38e00000` | Vectored Interrupt Controller 0 | `0x200005a0` |
| VIC1 | `0x38e01000` | Vectored Interrupt Controller 1 | `0x200005a4` |
| VIC2 | `0x38e02000` | Vectored Interrupt Controller 2 | `0x200005b0` |
| GPIO Controller | `0x39700000` | Pin multiplexing and GPIO control | `0x200005c0` |
| USB PHY | `0x3c400000` | USB analog transceiver interface | `0x2000a760`, `0x2000a864` |
| PMGR / Clock | `0x3c500000` | Power management and clock gating | `0x200005bc` |
| Watchdog Timer | `0x3c800000` | System watchdog and reset control | `0x200000e4` |
| RSA Engine | `0x3d000000` | Public key modular arithmetic accelerator | `0x2000865c`, `0x20008f28` |
| Chip Configuration | `0x3d100004` | Chip ID and boot configuration strapping | `0x200005b8` |

## USB DWC2 Controller and DFU State Machine

The boot controller at `0x38400000` uses a Synopsys DesignWare USB 2.0 (DWC2) core.

### PHY and Controller Initialization
1. Maintain cable detect bit 0 and PHY-ready bit 5 in `0x3c400028`.
2. Program `GRSTCTL` (`0x38400010`): reset bit 0 must clear and core-ready bit 31 must set (`0x2000a864` - `0x2000a898`).
3. Sizing registers: `0x2000a8d4` writes `GRXFSIZ` at `0x38400024` with `0x218` (536 FIFO words). Adjacent writes configure device transmit FIFOs.

### Interrupt Status Transitions (`GINTSTS`, `0x38400014`)
The polling state machine at `0x2000a8fc` inspects `GINTSTS`:
- Bit 30 (`0x40000000`): With bit 0 = 1, enters state 2 (returns immediately). With bit 0 = 0, enters state 10, enabling USB device mode. Device initialization selects state 8 when bit 0 = 0.
- Bit 12 (`0x00001000`): USB reset detected.
- Bit 13 (`0x00002000`): Enumeration complete. Enters EP0 enumeration phase (`SET_ADDRESS`, `SET_CONFIGURATION(1)`).
- Bit 19 (`0x00080000`): OUT endpoint interrupt (`DAINT` bit 16 = 1, `DOEPINT0` bit 3 = 1 for SETUP token). Eight SETUP bytes are transferred via DMA to the receive buffer (`0x2000a2f0` - `0x2000a330`).
- Bit 18 (`0x00040000`): IN endpoint interrupt (`DAINT` bit 0 = 1). Handles response transmission and status stages (`0x2000a2a4` - `0x2000a2ec`).

### DFU Command Dispatcher (`0x20003e24`)
The DFU dispatcher parses USB SETUP packet byte 1. The DFU interface class is `0xfe` (registered via `0x20004738`).

| Request Byte | Command | Parameter Requirements | Action |
|---|---|---|---|
| `0x00` | `DETACH` | - | Detaches USB session |
| `0x01` | `DNLOAD` | Bytes [2:4] block number, [6:8] LE length (<= `0x800`) | Streams boot image chunks |
| `0x02` | `UPLOAD` | - | Uploads status/data |
| `0x03` | `GETSTATUS` | Length must be 6 | Completes manifestation |
| `0x04` | `CLRSTATUS` | - | Clears error condition |
| `0x05` | `GETSTATE` | Length must be 1 | Returns current DFU state |
| `0x06` | `ABORT` | - | Aborts ongoing transfer |

A zero-length `DNLOAD` writes `1` to `transfer+0x10` at `0x20003f24` to mark transmission complete. Manifestation `GETSTATUS` sets `state+0x29 = 1` at `0x200042f0`. The subsequent OUT status completion transitions `state+0x29` from `1` to `2` at `0x2000a370` - `0x2000a378`.

## Image Header and Verification

Boot image verification occurs after transfer drain (`0x20003bc8` -> `0x200007d4`). The download payload begins at offset `+0x400` relative to the downloaded container.

### Header Format (`+0x00` to `+0x400`)

| Offset | Size | Field | Description |
|---|---|---|---|
| `+0x00` | 4 bytes | Magic | ASCII `"8740"` (`0x38 0x37 0x34 0x30`), checked at `0x2000079c` |
| `+0x07` | 1 byte | Image Type | `3` (encrypted payload) or `4` (plain payload), checked at `0x20000814` |
| `+0x08` | uint32 | Entry Offset | Must be `<= P` where `P = align_up(payload_length, 16)` |
| `+0x0c` | uint32 | Payload Length | Little-endian byte count of executable image |
| `+0x10` | uint32 | Body Length | Must be `>= P + 0x300` (`0x20000824`) |
| `+0x14` | uint32 | Certificate Offset | Must be `>= P + 0x80` and 16-byte aligned (`0x2000085c`) |
| `+0x18` | uint32 | Certificate Length | Byte count of X.509 certificate chain |
| `+0x40` | 16 bytes | Auth Tag | AES digest verification tag (`0x200006ac`) |

### Verification Pipeline
1. Transfer CRC: `DNLOAD` maintains a reflected CRC-32 (polynomial `0xedb88320`, initial seed `0xffffffff`). The trailing four bytes of the download must equal the running CRC (`0x200040e0` - `0x20004194`).
2. Header Authentication: `0x200006ac` hashes header bytes `[0x00, 0x40)` with SHA-1 (`0x38000000`), processes the digest through the AES engine (`0x38c00000`), and compares the first 16 bytes against `header+0x40`.
3. Payload Digest: SHA-1 engine hashes the entire `0x400`-byte header followed by `P` bytes of payload (`0x20000870` - `0x200008e8`).
4. RSA Verification: The 128-byte (1024-bit) RSA signature resides at offset `payload + P`. Modular exponentiation is computed by the PKA engine at `0x3d000000` (`0x2000865c`). The result is checked against PKCS#1 v1.5 padding and verified against the certificate chain (`0x20005b90`, `0x20005e38`).
5. Decryption: If image type is 3, payload bytes are decrypted in place via AES hardware (`0x38c00000`) before jumping to the entry point.
