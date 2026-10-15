#!/usr/bin/env python3
"""extract_rsrc.py -- pull the resource partition out of an iPod nano 7G IPSW.

  extract_rsrc.py IPSW OUT.img1

Writes the "rsrc" image of the IPSW's Firmware.MSE (plaintext FAT16
"iPodResources" behind a 0x400-byte img1 header) exactly as it sits on the
device's firmware area, for run.sh --nand-rsrc OUT.img1 (FTL LBA 0x3f2 on).
The snap6 OS is 1.1.2 (39A10023): use iPod_1.1.2_39A10023.ipsw (its
Resources/UI/SilverImagesDB.LE.bin is 33,317,720 bytes, matching the RAM)."""
import struct
import sys
import zipfile

MSE_DIR = 0x5000                # 16 x 40-byte image slots


def main():
    ipsw, out = sys.argv[1], sys.argv[2]
    with zipfile.ZipFile(ipsw) as z:
        name = next(n for n in z.namelist() if n.endswith('Firmware.MSE'))
        with z.open(name) as f:
            f.seek(MSE_DIR)
            slots = [f.read(40) for _ in range(16)]
            for s in slots:
                if s[4:8][::-1] != b'rsrc':
                    continue
                dev_off, length = struct.unpack('<II', s[12:20])
                f.seek(dev_off + 0x1000)
                left = length + 0x800   # length excludes the img1 overhead
                with open(out, 'wb') as o:
                    while left:
                        b = f.read(min(left, 1 << 20))
                        if not b:
                            break
                        o.write(b)
                        left -= len(b)
                print('rsrc: MSE dev_offset 0x%x, %d bytes -> %s' %
                      (dev_off, length + 0x800 - left, out))
                return
    sys.exit('no rsrc image in ' + ipsw)


if __name__ == '__main__':
    main()
