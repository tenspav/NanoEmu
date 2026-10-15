# FM Radio Tuner Hardware & Driver Analysis (dumps/snap6/ram-full.bin)
1. Strings: si47, bcm43, qcom, n76e have 0 hits. Tuner is integrated in Broadcom BCM2076B1/BCM2078 combo.
- 'bcm2076' off=0x0090cbfa (0x0890cbfa): "BCM2076B1 Vincent Class 1.5 CodePatches"
- 'radio' off=0x0000a66c (0x0800a66c): "RDS Robot Enabled: Radio OFF"; off=0x004bbdf4: "RadioTask"
- 'fm ' off=0x00036630 (0x08036630): "Supporting service FM Radio"; off=0x002c5d70: "We have a FM event..."
- 'tuner' off=0x001b34c0 (0x081b34c0): "Tuner Used : "; off=0x00793f2c: "TRFTuner_RTXC" (vtable 0x087ed420)
- 'rds' off=0x0000a70c (0x0800a70c): "RDS Event: Radio STOPPED"; off=0x00285818: "MeCCABufferedRDSUpdateTask"
- '88.' off=0x00615118 (0x08615118); 'mhz' off=0x0000a570 (0x0800a570); 'station' off=0x00033114 (0x08033114)
2. Hardware Bus & Protocol: NO I2C/SPI slave tuner. Tuner is integrated in Broadcom BCM2076/2078.
- Bus: Bluetooth UART at MMIO 0x3db00000 (descriptor 0x0891811c, 2.4MBaud), HCI H4 framing.
- Protocol: Broadcom HCI Vendor-Specific Command (VSC) opcode 0xFC15 (HCI_VSC_FM_TUNER).
- TX Path: 0x080da23c (packs freq - 64000 kHz) -> 0x08416b68 (enqueue) -> 0x0854ce54 (emits VSC 0xFC15).
- RX Path: 0x080da40c (checks HCI evt 0x0E opcode 0xFC15 / evt 0xFF) -> 0x084ea138 -> fmEventCb 0x082c59c0.
3. Tasks / Threads:
- RadioTask: Thread ID 21 (0x15), entry 0x08566d08 (Thumb 0x08566d09), TCB 0x08a86420, prio 7, event 0x42.
- RadioModelAsyncTask: created 0x08014764, entry 0x080147a4, prio 16, stack 24KB; RDS Task: entry 0x0816a9c0, prio 9.
4. Requirements for QEMU / Homebrew:
- QEMU: Implement UART 0x3db00000; handle HCI VSC 0xFC15 (power, tune, scan), respond with evt 0x0E (cmd complete) and evt 0xFF sub-evt 0x12 (RDS blocks); route FM I2S audio stream.
- Homebrew: Send raw H4 HCI packets (type 1, opcode 0xFC15) to UART 0x3db00000 to tune chip directly; in OS, signal RadioTask via Event 0x42 / state struct 0x0890bc70 or call BT::FmRadioProfile at 0x082c5000.
