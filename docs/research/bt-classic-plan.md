# iPod nano 7G (S5L8747) Classic BT HCI Host Channel Architecture
## 1. UART Bring-Up Spec (MMIO 0x3db00000, init @ 0x08550b44 & config @ 0x08550d4c)
- Base 0x3db00000 (UART1): +0x00 UART_CTRL0=0x03 (TX/RX enable); +0x04 UART_CTRL1=0x0405 (FIFO enable, |=0x0c DMA mode); +0x08 UART_CTRL2=0x07 (8N1).
- Flow control & clock: +0x0c UART_CTRL3=0x01 at bootstrap, set to 0x11 (RTS/CTS HW flow control) at 0x08550d8a. Baud divisor written to +0x28 (UART_BAUD_DIV) via clock 5 (0x0842a1c0) / baud (0x08001c40): Bootstrap=115,200; Operational=2,400,000 (2.4 MBaud).
## 2. Minimal HCI Sequence & Concrete Packet Templates (H4: 01=Cmd, 02=ACL, 04=Evt)
- Init: HCI_Reset `01 03 0C 00` -> Broadcom Baud 2.4M `01 18 FC 06 00 00 00 9F 24 00` (switch host UART to 2.4M) -> Set BD_ADDR `01 01 FC 06 <6B_ADDR>`.
- Discoverable: HCI_Write_Scan_Enable `01 1A 0C 01 03` (Inquiry+Page Scan). Optional: Local Name `01 13 0C F8 <Name>`, Class of Dev `01 24 0C 03 04 04 24`.
- SSP Auto-Accept: Write Simple Pairing Mode `01 56 0C 01 01`. On Evt 0x31 (IO Cap Req): reply `01 2B 04 09 <6B_ADDR> 03 00 00` (0x03=NoInputNoOutput / Just Works). On Evt 0x33 (User Conf Req): reply `01 2C 04 06 <6B_ADDR>` (Auto-accept; native OS 0x082c8494).
- L2CAP Conn Req Template (Handle 0x0040, SCID 0x0040): `02 40 20 0C 00 08 00 01 00 02 01 04 00 <PSM_L> <PSM_H> 40 00`.
- PSM Selection: PSM 0x0003 (`03 00`) for RFCOMM/SPP stream; PSM 0x0013 (`13 00`) for HID Gamepad; PSM 0x1001 (`01 10`) for raw fast L2CAP file transfer.
## 3. OS Hook Points for Homebrew
- Direct MMIO: Poll +0x10 (TX FIFO not full), write byte to +0x20 UART_TX_DATA; read +0x24 UART_RX_DATA.
- OS TX Paths: Call built-in HciUartTransmit at 0x08133ed4(buf, len) or low-level UartWrite at 0x08410820(handle, buf, len, 0xbb8) with handle from 0x0896d8e0+0x14. Post OS HCI commands directly via 0x080f731e(opcode, len, ptr).
- OS RX Hook: Hook call to 0x080f8640(buf, len) at 0x081920f8 in 'BT Uart RxLoop' (0x081920ac) to intercept incoming H4 packets from the 32KB DMA ring buffer.
## 4. Emulator Path (QEMU 0x3db00000 -> Host Chardev Socket)
- QEMU Model: Implement SysBus UART at 0x3db00000 with CharBackend `chr`. TX (+0x20): `qemu_chr_fe_write_all(&s->chr, &byte, 1)`. RX (+0x24): `qemu_chr_fe_set_handlers()` pushes bytes into guest FIFO and triggers UART interrupt.
- QEMU CLI: `-chardev socket,id=bt0,path=/tmp/nano-bt.sock,server=on,wait=off -global s5l8747.bt_chardev=bt0`.
- Host Companion: Python daemon connects to `/tmp/nano-bt.sock`, emulates BCM2076B1 responses (0x0E complete for 0x0C03/0xFC18/0x0C1A), or bridges to Linux `/dev/vhci` (virtual BlueZ controller) to pair against host Bluetooth tools.
## 5. 4-Week Milestone Breakdown
- Week 1 (Virtual UART Bring-Up): QEMU 0x3db00000 chardev socket; verify HCI_Reset `01 03 0C 00` loopback and 2.4M baud switch via Python mock.
- Week 2 (Pairing & Link Setup): Broadcom VSC handling (0xFC18, 0xFC01); pass Scan Enable `01 1A 0C 01 03` and complete host SSP auto-accept via BlueZ vhci.
- Week 3 (L2CAP Data Pipe): Establish L2CAP connection (PSM 0x1001 / RFCOMM PSM 0x0003); benchmark raw ACL throughput (target 1.2-1.6 Mbps) for file transfers.
- Week 4 (Homebrew App Demo): Deploy on-device hooks (0x08133ed4 / 0x081920f8); demo companion file transfer client and HID Gamepad report demo (PSM 0x0013).
