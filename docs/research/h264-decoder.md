Target: Apple M2 H.264 Video Decoder MMIO Analysis (/home/pavliukov/ipodwork/NanoRework/dumps/snap6/ram-full.bin)
1. Driver Strings & Code References (Pointers in Little-Endian):
- "AppleM2H264MPDDriver picture buffer list...": str 0x0871b570, ref at 0x083e34d4 (loaded at 0x083e34a0)
- "H264_MPD::SwResetEntirely()": str 0x0871b274, ref 0x08495870; "SwResetForCore()": str 0x0871b2bc, ref 0x08494fa0
- "H264_MPD::ParsingStage()": str 0x0871b3c4, ref 0x08493d3c; "frame_decode_complete()": str 0x0871b490, ref 0x08496be4
- "H264_DecoderDriver::decodeFrame()": strings 0x0871b620/72c/838/944, refs at 0x08232ca4, 0x08232ca8, 0x08232cb0, 0x08232cb4
- AppleM2H264MPDDriver vtable: 0x0879e9d0, refs at 0x08065e6c, 0x081d7748, 0x083ddffc, 0x083e3558
2. MMIO Register Map (0x39600000 - 0x39670000 accessed by driver):
- 0x39600000: Top IRQ status/W1C (bit 6: blk 66, bit 8: blk 61, bit 1: blk 63, 0x080ec8d8); +0x04: mode (0x142); +0x0c: pwr/en (0=rst, 1=en)
- 0x39600010: ctrl (0x80000000); +0x1c: init (0xffffffff); +0x2c: init (2); +0x1004: SwReset (poll bit 0 -> 0); +0x10d8: reset bits
- 0x39610000: Sub-blk 1 IRQ W1C (0xffffffff/4, 0x0812ebbc); +0x04: cfg (0x3f); +0x0c: run (12); +0x10/14/1c/34/9c/a0: bitstream params
- 0x39630004/20: status poll; 0x39630100: IRQ W1C/arm (0x20); 0x39630104: ctrl (20); 0x39630110: Doorbell (code 0x0812ebee)
- 0x39641200/1300: scaling lists (64 words each, 0x0814e76e-78); 0x39641804: config (code 0x0812eaf0)
- 0x39650000: status/W1C; 0x39650010: core profile (0xc02 | flags, code 0x0812eafe)
- 0x39660000: Frame IRQ status/W1C (bit 1: decode done, 0x080ec8f6); +0x04: init (0x12); +0x0c: start decode (3, 0x0812ebf6)
- 0x39660010: mode (0x200); +0x6c: geom (0x0815a648); +0x78/7c/80: aux MV bufs; +0x2c/34: Ref0 Y/CbCr; +0x3c/44: Ref1 Y/CbCr
- 0x39660018: Dest Y / bitstream in addr (0x0814e6f6, 0x0812ebdc); +0x1c: bitstream size; +0x20/24: Dest Cb/Cr addrs (0x0814e6fe)
3. Decode-Job Submission & Completion:
- Buffer Layout: In bitstream at 0x39660018/1c. Out: Y (0x39660018), Cb (0x39660020), Cr (0x39660024); Ref0/1 at +0x2c/+0x3c (bit 31 masked).
- Doorbell Register: 0x39630110 (write 2 or 0x102 at 0x0812ebee triggers HW, followed by 0x3966000c = 3 at 0x0812ebf6).
- Completion Signaling: HW sets 0x39660000 bit 1, 0x39600000 bit 6; raises IRQ 45. ISR (0x080ec8cc) posts sem at 0x08811d50 via 0x08427a66.
- IRQ Number: IRQ 45 (Bank 1 bit 13, writes 0x2000 to glue 0x38e0200c; VIC table entry at 0x08ae67b8+8+4*45 = 0x08ae6874 -> handler 0x080ec8cd).
4. Minimal QEMU Model Logic (Driver Init + 1 Frame Accepted):
- Read 0x39601004: return 0 (reset done). Read 0x39630004/20: return 0. Reads to other unmodeled registers return 0.
- Read 0x39600000: return s->irq_top (0x40 when IRQ pending). Write 0x39600000: s->irq_top &= ~val; if (!s->irq_top) qemu_set_irq(s->irq, 0);
- Read 0x39660000: return s->irq_sub (0x02 when done). Write 0x39660000: s->irq_sub &= ~val; Write 0x39610000/39630100/39650000: ignore.
- Write 0x39630110 (doorbell) or 0x3966000c (start decode): s->irq_sub |= 0x02; s->irq_top |= 0x40; qemu_set_irq(s->irq, 1);
