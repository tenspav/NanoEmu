GPU Investigation Report (/home/pavliukov/ipodwork/NanoRework/dumps/snap6/ram-full.bin)
1. Strings (Offset / Runtime Addr / String):
- 0x00105e2c (0x08105e2c): "MBX1" (PowerVR MBX engine identifier)
- 0x001dd104 (0x081dd104): "MBXFlushThread" (heap: 0x0197fa84 / 0x0997fa84)
- 0x00254f14 (0x08254f14): "MBXFlipHandlerThread" (heap: 0x01982298 / 0x09982298)
- 0x0047f88c (0x0847f88c): "GKMBX2D" (heap: 0x01984bdc / 0x09984bdc)
- 0x00808650 (0x08808650): "_disable_mbx_timeout"
- 0x0071b570 (0x0871b570): "* AppleM2H264MPDDriver picture buffer list has %d members..."
- 0x0071b274 (0x0871b274): "H264_MPD::SwResetEntirely() Could not perform software reset..."
- 0x0071b3c4 (0x0871b3c4): "H264_MPD::ParsingStage() Could not perform software reset..."
2. Code Cross-References (Little-endian addresses):
- "MBX1": 0x08105e20 (PC-relative ADR)
- "MBXFlushThread": 0x081dd100 (func ptr: 0x081dd021), 0x08a678a8, 0x092d4edc
- "MBXFlipHandlerThread": 0x08254f10 (func ptr: 0x08254ea3), 0x08a678a0, 0x092d4ed4
- "GKMBX2D": 0x0847f888 (func ptr: 0x084806e3), 0x08a67898, 0x092d4ecc
- "_disable_mbx_timeout": 0x08753e48
- "AppleM2H264MPDDriver": 0x083e34d4
- "H264_MPD::ParsingStage": 0x08493d3c
3. Unknown MMIO Bases Identified (GPU / Video Hardware):
- 0x39100000, 0x39200000, 0x39300000: PowerVR MBX Lite 2D/3D GPU core registers
- 0x39600000: Apple M2 H.264 multi-pipe video decoder (sub-blocks 0x3960/61/63/64/65/660000)
