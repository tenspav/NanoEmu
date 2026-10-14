# S5L8740/S5L8747 Bluetooth Architecture and Driver Reference

The Bluetooth subsystem pairs a Broadcom BCM2076B1 controller with an integrated HCI/L2CAP protocol stack running on the application processor. Physical transport uses a dedicated high-speed UART interface mapped at MMIO base `0x3db00000`, supporting both Classic Bluetooth (BR/EDR) audio and profiles and Bluetooth Low Energy (BLE).

## Hardware Controller and UART Interface

The physical controller is a Broadcom BCM2076B1 transceiver, identified by ROM firmware string `"BCM2076B1 Vincent Class 1.5 CodePatches"` at addresses `0x0890cbfa` and `0x0b38e696`.

### Dedicated Bluetooth UART (`0x3db00000`)
The Bluetooth transport backend descriptor at `0x0891811c` specifies a dedicated high-speed UART core mapped at `0x3db00000` (stored at offset `+0x0c`).

| Offset | Name | Description |
|---|---|---|
| `+0x00` | UART_CTRL0 | Primary control register |
| `+0x04` | UART_CTRL1 | Secondary control and FIFO enable |
| `+0x08` | UART_CTRL2 | Line configuration |
| `+0x0c` | UART_CTRL3 | Flow control and handshake |
| `+0x10` | UART_STATUS0 | FIFO occupancy and line status |
| `+0x18` | UART_STATUS1 | Interrupt pending flags |
| `+0x20` | UART_TX_DATA | Transmit data register |
| `+0x24` | UART_RX_DATA | Receive data register |
| `+0x28` | UART_BAUD_DIV| Baud rate divisor |

### Baud Rates and Clocks
- Bootstrap Baud Rate: Initial communication uses 115,200 baud.
- Operational Baud Rate: Fast transfer runs at 2,400,000 baud, configured by `0x0854f86c` -> `0x08172bcc` using UART index 1.
- SoC UART Routing: A distinct peripheral UART selector at `0x083cd2c0` manages ports 0 through 3 (`0x3c300000`, `0x3ce00000`, `0x3d200000`, `0x3da00000`) with IRQs 9, 10, and 11 (`0x08000c88`, `0x08000c8e`, `0x08000c94`). The Bluetooth UART at `0x3db00000` operates as a separate dedicated port.

## HCI Driver and Vendor Protocol

The primary HCI driver dispatch table resides at `0x0896d8fc`:
- Initialization routine: `0x08140ae0`
- Teardown routine: `0x0815d50c`
- Packet transmit routine: `0x08133ed4`
- Default state descriptor: `0x0896d8e0` (points to `"NULL Transport"` at `0x0895c100` via `0x0895c134` when inactive).

### Packet Framing (`0x0854f882`)
The transport layer uses standard HCI H4 serial framing:

| Indicator Byte | Type | Description |
|---|---|---|
| `0x01` | HCI Command | Outgoing command packet from host to controller |
| `0x02` | HCI ACL Data | Bidirectional data packets (supports 16-bit payload length headers) |
| `0x03` | HCI SCO Data | Synchronous voice audio channel data |
| `0x04` | HCI Event | Incoming event packet from controller to host |

### Broadcom Vendor Commands
During hardware initialization (`0x0854f56c`), the driver issues vendor-specific HCI commands:
- `0xfc18`: Broadcom patchram download and RAM patching
- `0xfc01`: Broadcom chip reset and core initialization
- `0xfc27`: Radio frequency calibration and baseband clock setup

## System Tasks and Software Architecture

The operating system manages the Bluetooth stack across several cooperative threads:

| Task Name | Descriptor Address | Role |
|---|---|---|
| `"BT StackMgr"` | `0x0802ed8c` | Stack state management and lifecycle control |
| `"BT CoreStack"` | `0x080f9f98` | Core packet processing and dispatch |
| `"BT ServiceMgr"` | `0x08036654` | Service discovery and registration |
| `"BT L2CAP TXLoop"` | `0x080ce6dc` | L2CAP packet transmission loop |
| `"BT Uart RxLoop"` | `0x08140b80` | UART byte stream reception (entry `0x081920ac`) |

Bridge initialization `BluetoothOSBridgeInit` (`0x080cf500`) calls hardware initialization `0x0854f56c`. Upon completion, `0x084e67fc` initializes the HCI core (`0x080f949c`) and L2CAP core (`0x080f7740`).

### Classic Audio and Remote Services
- A2DP Service: `"BT A2DPService"` (`0x08044c94`) and transmission worker `"BT A2DP Send Thread"` (`0x0804cbbc`).
- Remote Control (AVRCP): `"BT RemoteService"` (`0x08044cfc`).
- Protocol Descriptors: Registered for `"RFCOMM"` (`0x080fa7c4`) and `"L2CAP"` (`0x080fa868`).

## Pairing, Security, and BLE Support

### Pairing Manager (`"BT PairingMgr"`, `0x082c6894`)
- Legacy PIN Authentication: Handled at `0x082b97c4`.
- Secure Simple Pairing (SSP):
  - Outgoing auto-accept: `"Auto accepting outgoing SSP request."` at `0x082c73b0`.
  - Accept routine: `0x082c8494`.
  - Reject routine: `0x082c8410`.
- Persistent Link Keys: Stored in filesystem configuration `btdevices.plist` (referenced at `0x082c4fb4`).

### Bluetooth Low Energy (BLE) Engine
The stack supports concurrent BLE operation:
- ATT write routine: `0x080ea594`.
- Attribute serialization: `0x080e78d8`.
- GATT write execution runner: `0x084321c0`.
