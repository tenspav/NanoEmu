#!/usr/bin/env python3
"""disk_patch.py -- replace a file's contents on the emulated disk, without
touching the disk image.

  disk_patch.py IMG /FAT/PATH=LOCAL [/FAT/PATH2=LOCAL2 ...] -o PATCH
                [--part-block 63] [--lba-offset 49216]

IMG is the device dd (MBR + FAT32 partition at 4 KB block --part-block). Each
LOCAL file's bytes are laid over the clusters of the existing /FAT/PATH; it
must not be larger than that file, and is padded to its size, so the
directory entry (size, cluster chain) stays valid and the OS's cached FAT
state still matches. Homebrew HRL1 blobs (.hbapp) are padded inside the
format - zeros appended to the image section, image_size grown - because
the resident's loader (sdk/hb_reloc_file.c) requires file size == header +
image + relocations; the loaded result is the same (the loader zero-fills
image_size..span anyway). Other files are zero-padded at the end. Output: PATCH for run.sh --nand-patch, a list of
{u64 FTL LBA, 4096 bytes} records after an 8-byte header "N7PATCH1"; FTL LBA
= image block + --lba-offset (49216 on the snap6 device)."""
import argparse
import struct
import sys

BLK = 4096


class Fat32:
    def __init__(self, f, part_block):
        self.f = f
        self.base = part_block * BLK
        bs = self.read(0, 512)
        self.bps = struct.unpack_from('<H', bs, 11)[0]
        self.spc = bs[13]
        rsv = struct.unpack_from('<H', bs, 14)[0]
        nfats = bs[16]
        fatsz = struct.unpack_from('<I', bs, 36)[0]
        self.root = struct.unpack_from('<I', bs, 44)[0]
        self.fat_off = rsv * self.bps
        self.data_off = (rsv + nfats * fatsz) * self.bps
        self.csize = self.spc * self.bps

    def read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def chain(self, c):
        out = []
        while 2 <= c < 0x0ffffff8:
            out.append(c)
            c = struct.unpack('<I', self.read(self.fat_off + 4 * c, 4))[0]
            c &= 0x0fffffff
        return out

    def coff(self, c):                  # partition byte offset of cluster c
        return self.data_off + (c - 2) * self.csize

    def entries(self, dirc):
        data = b''.join(self.read(self.coff(c), self.csize)
                        for c in self.chain(dirc))
        lfn = {}
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                lfn = {}
                continue
            if e[11] == 0x0f:           # long-name piece
                seq = e[0] & 0x1f
                part = e[1:11] + e[14:26] + e[28:32]
                lfn[seq] = part.decode('utf-16-le', 'replace')
                continue
            short = e[0:8].decode('ascii', 'replace').rstrip()
            ext = e[8:11].decode('ascii', 'replace').rstrip()
            name = ''.join(lfn[k] for k in sorted(lfn)).split('\0')[0] \
                if lfn else (short + ('.' + ext if ext else ''))
            lfn = {}
            clus = struct.unpack_from('<H', e, 20)[0] << 16 | \
                struct.unpack_from('<H', e, 26)[0]
            yield name, clus, struct.unpack_from('<I', e, 28)[0], e[11]

    def lookup(self, path):
        c = self.root
        parts = [p for p in path.split('/') if p]
        for i, p in enumerate(parts):
            for name, clus, size, attr in self.entries(c):
                if name.lower() == p.lower():
                    if i == len(parts) - 1:
                        return clus, size
                    c = clus
                    break
            else:
                sys.exit('%s: %r not found' % (path, p))
        sys.exit('%s: is a directory' % path)


def pad_hrl1(blob, size):
    """grow an HRL1 blob {magic, entry, image_size, span, nrelocs, align}
    to exactly `size` bytes with zeros at the end of its image section"""
    magic, entry, image, span, nrel, align = struct.unpack_from('<6I', blob)
    extra = size - len(blob)
    if extra <= 0:
        return blob
    if extra % 4:
        sys.exit('HRL1 padding must be a multiple of 4 bytes (%d)' % extra)
    hdr = struct.pack('<6I', magic, entry, image + extra,
                      max(span, image + extra), nrel, align)
    return hdr + blob[24:24 + image] + b'\0' * extra + blob[24 + image:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('repl', nargs='+')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--part-block', type=int, default=63)
    ap.add_argument('--lba-offset', type=int, default=49216)
    a = ap.parse_args()
    recs = []
    with open(a.img, 'rb') as f:
        fat = Fat32(f, a.part_block)
        if fat.bps != BLK:
            sys.exit('expected 4 KB sectors, got %d' % fat.bps)
        for r in a.repl:
            path, local = r.split('=', 1)
            clus, size = fat.lookup(path)
            new = open(local, 'rb').read()
            if len(new) > size:
                sys.exit('%s: %d bytes > %s (%d bytes)' % (local, len(new),
                                                           path, size))
            if new[:4] == b'HRL1':
                new = pad_hrl1(new, size)
            new += b'\0' * (size - len(new))
            chain = fat.chain(clus)
            for i in range(0, size, BLK):
                c = chain[i // fat.csize]
                block = (fat.base + fat.coff(c) + i % fat.csize) // BLK
                data = new[i:i + BLK].ljust(BLK, b'\0')
                if i + BLK > size:      # keep the old slack after EOF
                    old = fat.read(fat.coff(c) + i % fat.csize, BLK)
                    data = data[:size - i] + old[size - i:]
                recs.append((block + a.lba_offset, data))
            print('%s: %d bytes from %s over %d blocks (FTL LBA 0x%x..)' %
                  (path, len(new), local, (size + BLK - 1) // BLK,
                   recs[-((size + BLK - 1) // BLK)][0]))
    with open(a.out, 'wb') as o:
        o.write(b'N7PATCH1')
        for lba, data in recs:
            o.write(struct.pack('<Q', lba) + data)
    print('wrote %s (%d blocks)' % (a.out, len(recs)))


if __name__ == '__main__':
    main()
