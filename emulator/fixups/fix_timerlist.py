#!/usr/bin/env python3
"""Rebuild the RetailOS kernel timer list torn by the non-atomic RAM dump.

Kernel object 0x0890b948: +0x1c = tick count, +0x28 = timer list head.
Thread table 0x08aad6a0 (79 x 60 B): thread+8 = its timer node (or 0).
Node: +0 next, +4 pprev (address of the previous link field), +8 expiry
tick. The list is sorted by expiry and NULL-terminated; the expiry walk at
0x862d5bc pops the head while (now - expiry) >= 0.

In ram-full-08000000.bin the nodes form a cycle (thread 3 -> 30 -> 19 -> 3)
with inconsistent pprev, so the kernel livelocks in that walk once tick
0x82d9 is reached. This script emits an os-poke file that relinks every
thread's node in expiry order.

usage: fix_timerlist.py <ram-full.bin> > timerlist.poke
(snap2: > snap2-timerlist.poke; check first with check_timerlist.py)
"""
import struct
import sys

BASE = 0x08000000
KERN = 0x0890b948
HEAD = KERN + 0x28
THREADS = 0x08aad6a0
NTHREADS = 79

d = open(sys.argv[1], 'rb').read()


def rd(a):
    return struct.unpack_from('<I', d, a - BASE)[0]


now = rd(KERN + 0x1c)
# Candidates: every thread's node plus anything reachable from the head
# (snap2: thread 67's node is on the list but not in its thread slot).
# Keep only armed nodes (+0x14 active flag, cleared by the expiry walk).
cand = {}
for i in range(NTHREADS):
    n = rd(THREADS + 60 * i + 8)
    if n:
        cand[n] = i
n = rd(HEAD)
while n and n not in cand.values() and len(cand) < 512:
    cand.setdefault(n, rd(n + 0x10))
    if rd(n) in cand:
        break
    n = rd(n)
nodes = sorted(((rd(n + 8) - now) & 0xffffffff, n, i)
               for n, i in cand.items() if rd(n + 0x14) == 1)

print('# kernel timer list rebuilt by fix_timerlist.py (tick 0x%x)' % now)
prev_link = HEAD
print('0x%08x 0x%08x' % (HEAD, nodes[0][1] if nodes else 0))
for k, (_, n, i) in enumerate(nodes):
    nxt = nodes[k + 1][1] if k + 1 < len(nodes) else 0
    print('# thread %d node 0x%08x expiry 0x%x' % (i, n, rd(n + 8)))
    print('0x%08x 0x%08x' % (n, nxt))
    print('0x%08x 0x%08x' % (n + 4, prev_link))
    prev_link = n
