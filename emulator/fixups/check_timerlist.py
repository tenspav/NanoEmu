#!/usr/bin/env python3
"""check_timerlist.py RAM -- walk the kernel timer list (see fix_timerlist.py)
and report cycles, bad pprev links, unsorted expiries and thread nodes
that are not on the list. Read-only."""
import struct, sys
BASE, KERN, THREADS, NT = 0x08000000, 0x0890b948, 0x08aad6a0, 79
d = open(sys.argv[1], 'rb').read()
rd = lambda a: struct.unpack_from('<I', d, a - BASE)[0]
now, head = rd(KERN + 0x1c), rd(KERN + 0x28)
owner = {rd(THREADS + 60 * i + 8): i for i in range(NT) if rd(THREADS + 60 * i + 8)}
print('tick 0x%x head 0x%08x, %d thread nodes' % (now, head, len(owner)))
seen, n, prev_link, last, bad = [], head, KERN + 0x28, None, 0
while n and n not in seen:
    seen.append(n)
    exp = rd(n + 8)
    rel = (exp - now) & 0xffffffff
    flags = []
    if rd(n + 4) != prev_link: flags.append('pprev=0x%08x!' % rd(n + 4)); bad += 1
    if last is not None and ((exp - last) & 0xffffffff) >= 0x80000000: flags.append('UNSORTED'); bad += 1
    if n not in owner: flags.append('no-owner')
    print('  node 0x%08x thr %s exp 0x%x (now%+d) %s' % (n, owner.get(n), exp,
          rel if rel < 0x80000000 else rel - 0x100000000, ' '.join(flags)))
    prev_link, last, n = n, exp, rd(n)
if n:
    print('CYCLE back to 0x%08x' % n); bad += 1
off = [(hex(k), v) for k, v in owner.items() if k not in seen]
print('thread nodes not on list:', off)
print('VERDICT:', 'TORN' if bad else 'consistent')
