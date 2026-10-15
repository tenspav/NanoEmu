1. Accessory Detect (Tristar / ResId): Tristar CONDET on I2C1 (0x3c900000, slave 0x1A), reg read 0x083d4c12.
   - Tasks: "TriStarCONDETProcessTask" (str 0x08010ec0, desc 0x08a93ccc, entry 0x08017590), "TriStarVBUSProcessTask" (0x08010ea0, entry 0x08017600).
   - GPIO MMIO 0x39700000, bank write 0x08426f7c (bank*0x20); power/mux config 0x08019264 toggles GPIO 0x1B/0x4A via 0x08426e80.
   - Legacy 30-pin Resistor ID: "_IapDoNotCountOnlyLineoutResIdAsConnection" (0x081eb08c), "_IapCountAllResIdAsConnection" (0x081eb070).
   - ResId check/auth fail str: "ERROR: iAP auth fail, bad resistor ID: %02d, eventIndexNext: %02d" (0x08667959).
   - Connection mgr ctor: 0x081eae34, vtable 0x087a0894, singleton 0x099c44ec in global state 0x0880f9e0 (+0x04).
2. Serial Protocol (iAP / Lingo):
   - Active ports table 0x08970330: Port 0 = CIapPortUsb (0x099bc500), Port 1 = CIapPortSerial (0x099bcac8), Port 2 = CIapPortSerial (0x099bd22c).
   - UART mapping: Port 1 uses UART1 (0x3ce00000, IRQ 10), Port 2 uses UART2 (0x3d200000, IRQ 11). UART selector: 0x083cd2c0.
   - CIapPortSerial ctor: 0x080bd318, vtable: 0x0879ec78 (vptr 0x0879ec7c). TX: 0x081d9658 -> 0x08183a18; RX/poll: 0x081d96b0 -> 0x0801ecea.
   - Framing & Sync: CIapPortVPort::HandleReceiveBuffer (0x081c96ac); sync FSM 0x081c94a4 validates iAP start byte 0x55 (0x081c94be).
   - Threads: "CIapIncomingProcessThread" (0x0804c500, ctor 0x0804c440), "CIapOutgoingProcessThread" (0x08246a10, ctor 0x082469f0).
   - Lingo dispatcher: table 0x08667820-0x08667920; CIapLingoGeneral vtable 0x087a2d28, CIapLingoDigitalAudio vtable 0x087ae418.
3. Line-Out Audio Control:
   - Codec: CS42L73 (ID 0x4281) on SPI0 (0x3c300000, IRQ 9, write 0x08429fb8); routing init 0x080d0220 (regs 0x0203-0x0207).
   - Volume control: 0x083b6a4c (sel 0 = Headphone regs 0x0227/0x0228; sel 1 = Line-Out regs 0x0229/0x022a at 0x083b6aba).
   - iAP Property: Lingo 8, Prop 0x000D at 0x08667900 -> "IPodMediaVolumeMgr::prLineOutVolume" (0x087f33a4), CIapPrefsLineOut (vtable 0x087a2e04).
   - MikeyBus 3.5mm lineout switch: "_mikeybus_lineout" (0x08808665, pref ID 0x7F at 0x08754490).
