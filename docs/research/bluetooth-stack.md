mentation](https://developer.apple.com/documentation/externalaccessory/)
28. Prior metadata describes a non-atomic live dump; no `w.py`/`drive.py` was found in the specified research folder. No files were written.
tokens used
152,308
**Verdict: classic Bluetooth pairing is supported by this firmware; a general-purpose classic data channel still needs integration.**
Addresses below are RAM VAs; Thumb function pointers carry bit 0.

1. **Chip/transport:** `0x0890cbfa`: `"BCM2076B1 Vincent Class 1.5 CodePatches"`, also copied at `0x0b38e696`.
2. Built-in **iPod HCI backend** table `0x0896d8fc`: init `0x08140ae0`, terminate `0x0815d50c`, write `0x08133ed4`.
3. Its init calls `0x0854f86c` → `0x08172bcc` with **UART index 1**, normal baud **2,400,000**; bootstrap uses **115,200**.
4. That backend’s UART descriptor `0x0891811c` contains MMIO base **`0x3db00000`** at `+0x0c`.
5. It accesses `+0x00/+0x04/+0x08/+0x0c` controls, `+0x10/+0x18` status, `+0x20` TX, `+0x24` RX, `+0x28` baud divisor.
6. A **separate UART map**, selector `0x083cd2c0`, maps indices 0/1/2/3 to `0x3c300000/0x3ce00000/0x3d200000/0x3da00000`.
7. IRQ registration `0x080001d8` confirms IRQ **9/10/11** → handlers **`0x08000c88/0x08000c8e/0x08000c94`**, each dispatching to `0x08000fec`.
8. Thus **fourth UART `0x3da00000` exists in the driver**; its IRQ was not established.
9. **Do not equate the two UART1 maps:** the traced BT backend uses `0x3db00000`; its own IRQ/handler linkage remains unresolved.
10. Snap6 has BT stopped: backend state `0x0896d8e0` is inactive; selected HCI table pointer `0x0895c134` → `0x0895c100`, named **`"NULL Transport"`**.
11. Consequently, these dumps establish available code paths, not a currently operating UART connection.

12. **Initialization:** `BluetoothOSBridgeInit` **`0x080cf500`** → hardware init **`0x0854f56c`**.
13. **HCI framing:** parser **`0x0854f882`** handles H4 types **1 command / 2 ACL / 3 SCO / 4 event**, including 16-bit ACL lengths.
14. Hardware init emits vendor opcodes **`0xfc18`**, **`0xfc01`**, **`0xfc27`**: standard HCI plus Broadcom initialization commands.
15. Tasks: `"BT StackMgr"` `0x0802ed8c`, `"BT CoreStack"` `0x080f9f98`, `"BT ServiceMgr"` `0x08036654`.
16. `"BT L2CAP TXLoop"` `0x080ce6dc`; `"BT Uart RxLoop"` `0x08140b80`, entry **`0x081920ac`**.
17. Classic services: `"BT A2DPService"` `0x08044c94`, `"BT RemoteService"` `0x08044cfc`, `"BT A2DP Send Thread"` `0x0804cbbc`.

18. **Full classic stack evidence:** startup completion **`0x084e67fc`** invokes HCI init `0x080f949c` and L2CAP init `0x080f7740`.
19. `"RFCOMM"` `0x080fa7c4`, `"L2CAP"` `0x080fa868`; RFCOMM naming alone does **not** establish an exposed SPP service.
20. Pairing: `"BT PairingMgr"` `0x082c6894`; `"Auto accepting outgoing SSP request."` `0x082c73b0`.
21. PIN request `0x082b97c4`; SSP accept/reject `0x082c8494/0x082c8410`; persistent classic keys: `btdevices.plist` `0x082c4fb4`.
22. This is **classic + BLE**, not a BLE-only controller or OS path.

23. **Shortest classic channel:** reuse native pairing/security and add a custom **BR/EDR L2CAP channel**; investigate RFCOMM if the host requires serial compatibility.
24. Raw HCI ACL requires managing handles, fragmentation and controller credits; it is a lower-level integration path, not an application socket.
25. **Shortest already-demonstrated channel:** optimize existing GATT notifications/write-without-response, batching, connection parameters and application flow control.
26. Previous notes identify ATT write **`0x080ea594`**, serializer **`0x080e78d8`**, GATT write runner **`0x084321c0`**; larger negotiated MTU remains unverified.
27. If the host is an iPhone, verify its available classic API first: Apple’s External Accessory route requires MFi accessories. [Apple documentation](https://developer.apple.com/documentation/externalaccessory/)
28. Prior metadata describes a non-atomic live dump; no `w.py`/`drive.py` was found in the specified research folder. No files were written.
