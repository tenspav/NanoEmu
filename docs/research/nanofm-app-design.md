# NanoFM: FM Radio Player Homebrew Design & HCI Protocol (iPod nano 7G: BCM2076B1 + S5L8747)
1. HARDWARE ARCHITECTURE & AUDIO ROUTING (Reverse-engineered from snap6 ram-full.bin @ 0x080d0220):
   - Tuner: BCM2076B1 FM core via BT UART 0x3db00000 (IRQ 25, 2.4MBaud, HCI H4 type 1, VSC opcode 0xFC15).
   - Audio Routing: ANALOG direct pass-through (NOT I2S). BCM FM analog out connects to Cirrus CS42L81 Line-In (AIN).
     CS42L81 over SPI0 (0x3c300000): Reg 0x000d=0x02/0x000e=0x40 (power on Line-In/bypass), Reg 0x011f/0x0120=0x3f (gain),
     Reg 0x012e=0xaa (connect Line-In direct to HP amp); 0% CPU DMA load! Stock OS MeCCA audio mixer is completely bypassed.
2. HCI VSC 0xFC15 COMMAND PACKETS (H4 type 1: 01 15 fc <len> <subcmd> ...; freq encoding = freq_kHz - 64000 [0xfa00] uint16_le):
   - Power ON FM (RX mode):   01 15 fc 03 12 01 02 (subcmd 0x12, p1=0x01 ON, p2=0x02 RX); Power OFF: 01 15 fc 03 12 00 00
   - Set Band (EU/US 87.5-108):01 15 fc 03 01 00 02 (subcmd 0x01; use 0x03 for Japan 76-90 MHz)
   - Route FM Audio (Analog): 01 15 fc 04 05 00 60 00 (subcmd 0x05, 0x60=analog out), 01 15 fc 03 4d 01 01 (unmute 0x4d)
   - Set Volume:              01 15 fc 03 08 00 ff (subcmd 0x08, 0xff=max audio gain)
   - Tune Frequency:          01 15 fc 04 0a 00 <freq_lo> <freq_hi> (subcmd 0x0a, [freq_kHz - 64000]_le)
     * 88.5 MHz  (88500 kHz  -> 24500 = 0x5FDC): 01 15 fc 04 0a 00 dc 5f
     * 101.1 MHz (101100 kHz -> 37100 = 0x90EC): 01 15 fc 04 0a 00 ec 90
     * 108.0 MHz (108000 kHz -> 44000 = 0xABF0): 01 15 fc 04 0a 00 f0 ab
   - Seek Band Limits:        01 15 fc 06 fb 00 f0 ab dc 5b (subcmd 0xfb, max=44000 [0xabf0], min=23500 [0x5bdc])
   - Seek Up / Down:          01 15 fc 03 09 00 01 (seek up), 01 15 fc 03 09 00 00 (seek down; subcmd 0x09)
   - Enable RDS & Event Mask: 01 15 fc 04 10 00 03 12 (subcmd 0x10, mask=0x03, rds_subevent=0x12)
3. RDS DECODING (BCM HCI Vendor Event 0xFF, sub-event 0x12):
   - Packet: 04 ff 0d 12 01 <blkA:2> <blkB:2> <blkC:2> <blkD:2> <rssi> (13 bytes total)
   - Extraction: When (blkB >> 11) == 0 (Group 0A/0B PS), offset = (blkB & 3) * 2; StationName[offset..+1] = blkD (ASCII).
4. APP UI: Top: freq banner ("101.1 MHz") + signal icon + 8-char RDS name. Mid: touch dial (87.5-108MHz) + Seek (<< / >>). Bottom: 6 presets (P1-P6).
5. NANOEMU TEST (nano7_btuart.c): Answer 0xfc15 with Cmd Complete: 04 0e 06 01 15 fc 00 <subcmd> 01 (status=0, subcmd echoed, substatus=1).
   RDS Test: Timer emits 04 ff 0d 12 01 ... with PS blocks "NANO-FM " to verify station name decoding in emulator.
