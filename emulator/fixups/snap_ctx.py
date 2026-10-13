#!/usr/bin/env python3
"""snap_ctx.py RAM CTX OUT -- build a resume ctx that matches a DumpRAM RAM image.

DumpRAM's ctx.bin can come from a different app launch than ram-full.bin
(snap2: ctx.bin pc 0x09352240 points at framebuffer pixels in the RAM image).
The RAM image itself holds the matching capture in the live app's g_ctx
(.bss, app_base+0x10e48). The live app is located via the capture code
(stmia.w r9,{r0-r12}; str.w sp,[r9,#52]) whose code base +0x4fb appears as
a return address on the captured stack. The saved pc (= mov r0,pc + 4, mid-
instruction) is replaced by the next boundary, the 'mrs r0,cpsr' at
base+0x2d2, with bit0 set (Thumb app; MRS reads T as 0)."""
import struct, sys
B = 0x08000000
ram = open(sys.argv[1], 'rb').read()
ctx = struct.unpack('<18I', open(sys.argv[2], 'rb').read(72))
sp = ctx[13]
stack = struct.unpack_from('<128I', ram, sp - B)
sig = bytes.fromhex('89e8ff1fc9f834d0')          # app off 0x2c0
i, live = ram.find(sig), None
while i >= 0:
    base = B + i - 0x2c0
    if (base + 0x4fb) in stack:
        live = base
    i = ram.find(sig, i + 1)
if live is None:
    # v3 build (snap4): different code layout, but ctx.bin == the in-RAM g_ctx.
    # Its saved pc (mov r0,pc in Thumb = +4) is already the mrs address.
    g = ram.find(struct.pack('<18I', *ctx))
    if g < 0 or ram[ctx[15] - B:ctx[15] - B + 4] != bytes.fromhex('eff30080'):
        sys.exit('live app instance not found')
    regs = list(ctx)
    regs[15] |= 1
    open(sys.argv[3], 'wb').write(struct.pack('<18I', *regs))
    print('v3: g_ctx 0x%08x == ctx.bin, pc 0x%08x (mrs)' % (B + g, regs[15]))
    sys.exit(0)
g = struct.unpack_from('<I', ram, live + 0x484 - B)[0]   # &g_ctx literal
regs = list(struct.unpack_from('<18I', ram, g - B))
assert regs[17] == 0x4d415244 and regs[15] == live + 0x2d0, regs
regs[15] = (live + 0x2d2) | 1
open(sys.argv[3], 'wb').write(struct.pack('<18I', *regs))
print('live app 0x%08x g_ctx 0x%08x pc 0x%08x (ctx.bin pc was 0x%08x)'
      % (live, g, regs[15], ctx[15]))
