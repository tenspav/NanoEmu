#!/usr/bin/env python3
"""torn_map.py torn-map.bin -- print the DRAM ranges DumpRAM v2 saw change.

Format (LE u32): 'TORN', version, block_bytes, n_blocks, then n_blocks x
{h0, hb, ha}: h0 = hash right after the ctx capture, hb/ha = just before/
after the block's own write. pre = h0 != hb, during = hb != ha."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
magic, ver, bs, n = struct.unpack_from('<4I', d)
assert magic == 0x4e524f54 and ver == 2, hex(magic)
runs, cur = [], None
for i in range(n):
    h0, hb, ha = struct.unpack_from('<3I', d, 16 + 12 * i)
    tag = ('P' if h0 != hb else '') + ('D' if hb != ha else '')
    if tag and cur and cur[1] == i and cur[2] == tag:
        cur[1] = i + 1
    elif tag:
        cur = [i, i + 1, tag]; runs.append(cur)
tot = sum(r[1] - r[0] for r in runs)
print('%d/%d blocks torn (%d KB); P=changed before its write, D=during' % (tot, n, tot * bs // 1024))
for a, b, t in runs:
    print('  0x%08x-0x%08x %-2s %d KB' % (0x08000000 + a * bs, 0x08000000 + b * bs, t, (b - a) * bs // 1024))
