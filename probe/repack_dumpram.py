#!/usr/bin/env python3
"""Append DumpRAM.app to the on-device AllApps-B.pack (slot B), preserving every
existing bundle byte-identical and in order (icon ids are positional). Mirror of
repack.py for the DumpROM install.

Usage:
    repack_dumpram.py [SRC_PACK] [OUT_PACK]

  SRC_PACK  live device pack to extend (install-time default below)
  OUT_PACK  where to write the rebuilt pack

At install time, run with the device mounted and no args (defaults apply):
    sudo python3 repack_dumpram.py          # /mnt/ipod/... -> install/AllApps-B.pack.new
Verify-only dry run (device absent), extend the staged 9-app pack:
    python3 repack_dumpram.py install/AllApps-B.pack.new install/AllApps-B.pack.dumpram-dryrun
"""
import struct, sys
from pathlib import Path
from packtool import parse_pack

DEV_PACK   = "/mnt/ipod/Apps/AllApps-B.pack"
NEW_BUNDLE = "install/DumpRAM.app"
OUT        = "install/AllApps-B.pack.new"

def align4(n): return (n + 3) & ~3

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEV_PACK
    out = sys.argv[2] if len(sys.argv) > 2 else OUT

    data = Path(src).read_bytes()
    _, apps = parse_pack(data)
    old_n = len(apps)
    blobs = [data[a["offset"]:a["offset"] + a["bundle_len"]] for a in apps]

    new = Path(NEW_BUNDLE).read_bytes()
    blobs.append(new)

    n = len(blobs)
    head = align4(12 + 4 * n)
    offs, blob, cur = [], bytearray(), head
    for b in blobs:
        offs.append(cur)
        blob += b
        blob += b"\0" * (align4(len(b)) - len(b))
        cur += align4(len(b))
    packed = bytearray(head) + blob
    struct.pack_into("<III", packed, 0, 0x4b504248, 1, n)   # HBPK v1
    for i, o in enumerate(offs):
        struct.pack_into("<I", packed, 12 + 4 * i, o)
    Path(out).write_bytes(packed)

    # verify: re-parse; the old bundles stay byte-identical and in order; DumpRAM last
    _, chk = parse_pack(bytes(packed))
    names = [a["name"] for a in chk]
    assert names[:old_n] == [a["name"] for a in apps], names
    assert names[old_n] == "DumpRAM" and chk[old_n]["sbid"] == "hb.dumpram"
    for i in range(old_n):
        o = chk[i]["offset"]
        assert bytes(packed)[o:o + chk[i]["bundle_len"]] == blobs[i], f"bundle {i} changed"
    ids = [a["app_id"] for a in chk]
    assert len(set(ids)) == len(ids), f"app_id collision: {[hex(x) for x in ids]}"
    print(f"OK: {out} — {n} apps, {len(packed)} bytes; order: {names}")
    print(f"    DumpRAM app_id=0x{chk[old_n]['app_id']:08x} kind={chk[old_n]['kind']}")

if __name__ == "__main__":
    main()
