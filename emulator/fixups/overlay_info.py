#!/usr/bin/env python3
"""overlay_info.py overlay.bin [ram-full.bin [torn-map.bin]] -- inspect a DumpRAM v4 overlay.

overlay.bin (LE u32): 'OVLY', ver, block_bytes, n, copy/window_us, w5, n_wanted,
n_missing/dropped (v1 = DumpRAM v4, w5 = saved irq mask; v2 = DumpRAM v5, IRQs on,
per-block verified copies, w5 = retries), then n DRAM base addrs, then n x block_bytes of data copied with
IRQs off at T0 (atomic with ctx.bin). With ram-full.bin: per block, how many
words the streamed image got wrong (= how torn it really was). With
torn-map.bin: torn blocks NOT covered by the overlay (still stale on resume).

overlay_info.py --make-test RAM TORN OUT: build a test overlay from the torn
blocks of RAM itself (identity data; the loader/exclusion self-test).
overlay_info.py --merge RAM OVL OUT: write RAM with the overlay applied (what the
emulator resumes from), e.g. for check_timerlist.py / snap_ctx.py."""
import struct, sys
B = 0x08000000


def torn_blocks(path):
    d = open(path, 'rb').read()
    _, _, bs, n = struct.unpack_from('<4I', d)
    return bs, [B + i * bs for i in range(n)
                if len(set(struct.unpack_from('<3I', d, 16 + 12 * i))) > 1]


if sys.argv[1] == '--make-test':
    ram = open(sys.argv[2], 'rb').read()
    bs, blks = torn_blocks(sys.argv[3])
    out = struct.pack('<8I', 0x594c564f, 1, bs, len(blks), 0, 0, len(blks), 0)
    out += struct.pack('<%dI' % len(blks), *blks)
    out += b''.join(ram[a - B:a - B + bs] for a in blks)
    open(sys.argv[4], 'wb').write(out)
    print('test overlay: %d blocks' % len(blks))
    sys.exit(0)

if sys.argv[1] == '--merge':
    ram = bytearray(open(sys.argv[2], 'rb').read())
    o = open(sys.argv[3], 'rb').read()
    bs, n = struct.unpack_from('<2I', o, 8)
    for k, a in enumerate(struct.unpack_from('<%dI' % n, o, 32)):
        ram[a - B:a - B + bs] = o[32 + 4 * n + bs * k:32 + 4 * n + bs * (k + 1)]
    open(sys.argv[4], 'wb').write(ram)
    print('merged %d blocks' % n)
    sys.exit(0)

d = open(sys.argv[1], 'rb').read()
magic, ver, bs, n, us, mask, want, miss = struct.unpack_from('<8I', d)
assert magic == 0x594c564f and ver in (1, 2), hex(magic)
assert len(d) == 32 + 4 * n + bs * n, 'truncated'
addrs = struct.unpack_from('<%dI' % n, d, 32)
print('v%d: %d blocks x %d KB (%d KB), wanted %d, dropped %d, window %d us, %s %d'
      % (ver, n, bs // 1024, n * bs // 1024, want, miss, us,
         'irq mask' if ver == 1 else 'retries', mask))
ram = open(sys.argv[2], 'rb').read() if len(sys.argv) > 2 else None
for k, a in enumerate(addrs):
    line = '  0x%08x' % a
    if ram:
        o = d[32 + 4 * n + bs * k:32 + 4 * n + bs * (k + 1)]
        r = ram[a - B:a - B + bs]
        diff = sum(1 for i in range(0, bs, 4) if o[i:i + 4] != r[i:i + 4])
        line += '  %6d words differ from ram-full' % diff
    print(line)
if len(sys.argv) > 3:
    _, tb = torn_blocks(sys.argv[3])
    left = [hex(a) for a in tb if a not in addrs]
    print('torn blocks not covered: %d %s' % (len(left), ' '.join(left)))
