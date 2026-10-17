#!/usr/bin/env python3
"""disk_import.py -- put new files into a folder of the emulated iPod's disk,
without touching the disk image: the result is a N7PATCH1 patch for run.sh
--nand-patch (the format of disk_patch.py).

  disk_import.py IMG FILE... -o PATCH [--dir /Apps/Data/Importar]
                 [--part-block 63] [--lba-offset 49216]

Each FILE gets a directory entry (long name + 8.3 alias) in --dir, a cluster
chain taken from the free clusters at the END of the volume (the OS's FAT
cache, captured in the RAM snapshot, holds the sectors it used last, near
the start and the recent writes; the end of the FAT is never in it), and
both FAT copies are updated. The FSInfo free count is left alone (the OS
keeps its own copy in RAM). The default folder is the one the NanoSync
homebrew app imports MP3s from."""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disk_patch import Fat32, BLK  # noqa: E402


def lfn_checksum(short):
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xff
    return s


def lfn_entries(name, short):
    u = name.encode('utf-16-le')
    chars = [u[i:i + 2] for i in range(0, len(u), 2)]
    n = (len(chars) + 12) // 13
    chars += [b'\0\0'] + [b'\xff\xff'] * (n * 13)
    chars = chars[:n * 13]
    ck = lfn_checksum(short)
    out = []
    for seq in range(n, 0, -1):          # stored last piece first
        part = chars[(seq - 1) * 13:seq * 13]
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == n else 0)
        e[1:11] = b''.join(part[0:5])
        e[11] = 0x0f
        e[13] = ck
        e[14:26] = b''.join(part[5:11])
        e[28:32] = b''.join(part[11:13])
        out.append(bytes(e))
    return out


def dos_time(t):
    lt = time.localtime(t)
    d = ((lt.tm_year - 1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday
    tm = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)
    return tm, d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('files', nargs='+')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--dir', default='/Apps/Data/Importar')
    ap.add_argument('--part-block', type=int, default=63)
    ap.add_argument('--lba-offset', type=int, default=49216)
    a = ap.parse_args()

    f = open(a.img, 'rb')
    fat = Fat32(f, a.part_block)
    if fat.bps != BLK:
        sys.exit('expected 4 KB sectors, got %d' % fat.bps)
    bs = fat.read(0, 512)
    fatsz = struct.unpack_from('<I', bs, 36)[0]
    nfats = bs[16]
    totsec = struct.unpack_from('<I', bs, 32)[0]
    nclus = (totsec - fat.data_off // fat.bps) // fat.spc + 2

    # the folder
    c = fat.root
    for p in [x for x in a.dir.split('/') if x]:
        for name, clus, size, attr in fat.entries(c):
            if name.lower() == p.lower() and attr & 0x10:
                c = clus
                break
        else:
            sys.exit('%s: %r not found' % (a.dir, p))
    dir_chain = fat.chain(c)
    dirdata = bytearray(b''.join(fat.read(fat.coff(x), fat.csize)
                                 for x in dir_chain))
    existing = {n.lower() for n, *_ in fat.entries(c)}
    shorts = {bytes(dirdata[i:i + 11]) for i in range(0, len(dirdata), 32)}

    # FAT, read lazily per 4 KB sector; changes collected here
    fatcache = {}

    def fat_sector(sec):
        if sec not in fatcache:
            fatcache[sec] = bytearray(fat.read(fat.fat_off + sec * BLK, BLK))
        return fatcache[sec]

    def fat_get(cl):
        s = fat_sector(cl * 4 // BLK)
        return struct.unpack_from('<I', s, cl * 4 % BLK)[0] & 0x0fffffff

    def fat_set(cl, v):
        s = fat_sector(cl * 4 // BLK)
        old = struct.unpack_from('<I', s, cl * 4 % BLK)[0]
        struct.pack_into('<I', s, cl * 4 % BLK, (old & 0xf0000000) | v)

    # free clusters from the end of the volume down
    def free_from_end():
        cl = nclus - 1
        while cl >= 2:
            if fat_get(cl) == 0:
                yield cl
            cl -= 1
    free = free_from_end()

    blocks = {}             # partition byte offset -> 4 KB block
    now = time.time()
    tm, dt = dos_time(now)
    for path in a.files:
        name = os.path.basename(path)
        if name.lower() in existing:
            print('skip (already there): %s' % name)
            continue
        data = open(path, 'rb').read()
        ncl = max(1, (len(data) + fat.csize - 1) // fat.csize)
        chain = [next(free) for _ in range(ncl)]
        chain.reverse()                 # ascending order on disk
        for i, cl in enumerate(chain):
            fat_set(cl, chain[i + 1] if i + 1 < ncl else 0x0fffffff)
            chunk = data[i * fat.csize:(i + 1) * fat.csize]
            chunk = chunk.ljust(fat.csize, b'\0')
            for j in range(0, fat.csize, BLK):
                blocks[fat.coff(cl) + j] = chunk[j:j + BLK]
        # 8.3 alias NSIMxxxx.MP3 / .EXT
        ext = os.path.splitext(name)[1][1:].upper().encode('ascii', 'replace')[:3]
        k = 0
        while True:
            short = b'NSIM%04d' % k + ext.ljust(3, b' ')
            if short not in shorts:
                break
            k += 1
        shorts.add(short)
        ents = lfn_entries(name, short)
        e = bytearray(32)
        e[0:11] = short
        e[11] = 0x20
        struct.pack_into('<HHH', e, 14, tm, dt, dt)
        struct.pack_into('<H', e, 18, dt)
        struct.pack_into('<H', e, 20, chain[0] >> 16)
        struct.pack_into('<HH', e, 22, tm, dt)
        struct.pack_into('<H', e, 26, chain[0] & 0xffff)
        struct.pack_into('<I', e, 28, len(data))
        ents.append(bytes(e))
        # a run of free slots (0x00 end marker or 0xe5 deleted)
        need = len(ents)
        i = 0
        run = 0
        pos = -1
        while i < len(dirdata):
            if dirdata[i] in (0x00, 0xe5):
                run += 1
                if run == need:
                    pos = i - (need - 1) * 32
                    break
            else:
                run = 0
            i += 32
        if pos < 0:
            sys.exit('no room left in %s (the folder cluster is full)' % a.dir)
        end_used = dirdata[pos + (need - 1) * 32] == 0x00
        for n, ent in enumerate(ents):
            dirdata[pos + n * 32:pos + (n + 1) * 32] = ent
        if end_used:                    # keep an end marker after us
            nxt = pos + need * 32
            if nxt < len(dirdata):
                dirdata[nxt] = 0x00
        existing.add(name.lower())
        print('add %s: %d bytes, %d clusters from %d, alias %s' % (
            name, len(data), ncl, chain[0], short.decode()))

    # directory clusters, both FATs
    for idx, cl in enumerate(dir_chain):
        for j in range(0, fat.csize, BLK):
            o = idx * fat.csize + j
            blocks[fat.coff(cl) + j] = bytes(dirdata[o:o + BLK])
    for sec, s in fatcache.items():
        if bytes(s) != fat.read(fat.fat_off + sec * BLK, BLK):
            for k in range(nfats):
                blocks[fat.fat_off + (k * fatsz + sec) * BLK] = bytes(s)

    with open(a.out, 'wb') as o:
        o.write(b'N7PATCH1')
        for off in sorted(blocks):
            lba = (fat.base + off) // BLK + a.lba_offset
            o.write(struct.pack('<Q', lba) + blocks[off])
    print('%s: %d blocks' % (a.out, len(blocks)))


if __name__ == '__main__':
    main()
