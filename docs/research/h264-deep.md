, +8 chroma>>10, +0xc auxiliary>>10, +0x10/+14/+18 physical pointers.
Surface getters 0x084132cc read surface+0x60/+0x6c; auxiliary comes from surface+0x30.
0x084964dc programs slot i=0..19: B+0x120c+4*i=Y>>10,
B+0x125c+4*i=chroma>>10; B+0x12ac+4*i=auxiliary>>10.
H.264 input parser: B+0x1200=input>>10; B+0x180c=input&0x3ff; B+0x1810=offset+NAL_length.
3. SPS/PPS and parser behavior
0x08232df2 accepts profiles 66/77/100, level<=31; avcC supplies 1–4-byte NAL lengths.
SPS/PPS initialization reads big-endian 16-bit lengths; decodeFrame 0x082328f8 reads sample NAL lengths.
0x08493b10 resets parser, writes input bounds, then B+0x1804|=1 and B+0x1600 low12=0x801.
B+0x1628 bit8 must become ready; bits[4:0]=NAL type, bits[6:5]=header error.
0x083c3560 requests Exp-Golomb parsing via B+0x1078=1; B+0x107c bit7 must become ready.
SPS storage starts 0x08990ddc, stride 0x838; decoded dimensions use SPS+0x454/+0x458/+0x45c.
4. Ordered H.264 initialization and completion
0x08232d22: 0x3e000004|=1 → install IRQ35 → allocate 0x4000/0x8000 work buffers.
Set B → B+0x1004|=0x7ff → wait bit0 clear → program work addresses shifted right10.
0x08496fa8 restores saved register ranges B+[0,0x340), [0x400,0xd00), [0x1200,0x1400).
0x08494890 programs slice/reference/scaling state, clears B+0x1078, then B+0x1000=1.
ISR clears B+0x10c0; B+0x10b8 bit4 marks malformed data; posts semaphore 0x08811d44.
Post-wait 0x08495b30 reads/acknowledges B+0x1074; happy result is 0 or 1.
Error bits1..7: VLC, macroblock, submotion, pattern, IDF, delta-Q, timeout.
5. Output, display, and implementation verdict
Two image pointers suggest Y plus combined chroma; NV12 order, tiling, and byte stride remain unverified.
Output addresses are dynamic; H.264 globals at 0x088117e0 and slot tables are zero in snap6.
Decoded surfaces return through 0x084971d8; no direct decoder→0x39000000/LCD linkage established.
LCD consumer 0x08080b70 programs 0x38900024 from surface->getBuffer()&0x7fffffff.
No decoder-register CRC check established; reset polls allow 10000 iterations; semaphore wait argument=0x84.
QEMU plan: implement 0x38f parser/readiness/reset/readback first; preserve register tables and IRQ35 completion.
Then integrate a host H.264 decoder with SPS/PPS, slice accumulation, DPB/reordering, and guest-plane writes.
Full playback is feasible in principle, but requires confirming output layout/display conversion; IRQ-only stubs cannot work.
```
tokens used
139,956
```text
snap6 verdict: the skeleton conflates two codecs; a complete H.264 output-layout spec remains unresolved.
1. Correct hardware identity
0x08226930/0x081958d6 recognize MPEG-4 start codes 0x1b0/0x1b6; 0x0814875c parses H.263.
These feed 0x396xxxxx through 0x08226c8e→0x0812eacc; IRQ45 belongs to this path.
H.264 init 0x08232cfc selects base B=0x38f00000 at 0x08232d8e→0x08496b94.
H.264 installs IRQ35 handler 0x080df2d9 at 0x08232d34; semaphore=0x08811d44.
The claimed vtable 0x0879e9d0 is a stream/file wrapper: methods 0x081d7638/7660 read/write files.
2. Submission and descriptors
Neither path passes a RAM descriptor to 0x39630110: 0x0812ebee writes command 2 or 0x102.
0x39660018=input+truncated_signed(header_bits/8); 0x3966001c=input+length+0x103.
Both mask bit31; +0x1c is an end address, not a size (0x0812ebc4–ebe2).
The 0x0814e6e8 setup record has five u32 fields: +0 Y/input, +4 chroma, +8 plane separation,
+0xc/+0x10 auxiliary pointers; writes 0x39660018/20/24/78/7c; 0x39660080=0.
H.264 software surface record at 0x08232f9a is 0x1c bytes:
+0 surface*, +4 Y>>10, +8 chroma>>10, +0xc auxiliary>>10, +0x10/+14/+18 physical pointers.
Surface getters 0x084132cc read surface+0x60/+0x6c; auxiliary comes from surface+0x30.
0x084964dc programs slot i=0..19: B+0x120c+4*i=Y>>10,
B+0x125c+4*i=chroma>>10; B+0x12ac+4*i=auxiliary>>10.
H.264 input parser: B+0x1200=input>>10; B+0x180c=input&0x3ff; B+0x1810=offset+NAL_length.
3. SPS/PPS and parser behavior
0x08232df2 accepts profiles 66/77/100, level<=31; avcC supplies 1–4-byte NAL lengths.
SPS/PPS initialization reads big-endian 16-bit lengths; decodeFrame 0x082328f8 reads sample NAL lengths.
0x08493b10 resets parser, writes input bounds, then B+0x1804|=1 and B+0x1600 low12=0x801.
B+0x1628 bit8 must become ready; bits[4:0]=NAL type, bits[6:5]=header error.
0x083c3560 requests Exp-Golomb parsing via B+0x1078=1; B+0x107c bit7 must become ready.
SPS storage starts 0x08990ddc, stride 0x838; decoded dimensions use SPS+0x454/+0x458/+0x45c.
4. Ordered H.264 initialization and completion
0x08232d22: 0x3e000004|=1 → install IRQ35 → allocate 0x4000/0x8000 work buffers.
Set B → B+0x1004|=0x7ff → wait bit0 clear → program work addresses shifted right10.
0x08496fa8 restores saved register ranges B+[0,0x340), [0x400,0xd00), [0x1200,0x1400).
0x08494890 programs slice/reference/scaling state, clears B+0x1078, then B+0x1000=1.
ISR clears B+0x10c0; B+0x10b8 bit4 marks malformed data; posts semaphore 0x08811d44.
Post-wait 0x08495b30 reads/acknowledges B+0x1074; happy result is 0 or 1.
Error bits1..7: VLC, macroblock, submotion, pattern, IDF, delta-Q, timeout.
5. Output, display, and implementation verdict
Two image pointers suggest Y plus combined chroma; NV12 order, tiling, and byte stride remain unverified.
Output addresses are dynamic; H.264 globals at 0x088117e0 and slot tables are zero in snap6.
Decoded surfaces return through 0x084971d8; no direct decoder→0x39000000/LCD linkage established.
LCD consumer 0x08080b70 programs 0x38900024 from surface->getBuffer()&0x7fffffff.
No decoder-register CRC check established; reset polls allow 10000 iterations; semaphore wait argument=0x84.
QEMU plan: implement 0x38f parser/readiness/reset/readback first; preserve register tables and IRQ35 completion.
Then integrate a host H.264 decoder with SPS/PPS, slice accumulation, DPB/reordering, and guest-plane writes.
Full playback is feasible in principle, but requires confirming output layout/display conversion; IRQ-only stubs cannot work.
```
