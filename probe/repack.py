#!/usr/bin/env python3
"""Append DumpROM.app to the on-device AllApps-B.pack (slot B), preserving the
existing 8 bundles byte-identical and in order (icon ids are positional).
Writes install/AllApps-B.pack.new. Verifies by re-parsing the result."""
import struct, sys
from pathlib import Path
from packtool import parse_pack

DEV_PACK = "/mnt/ipod/Apps/AllApps-B.pack"
NEW_BUNDLE = "install/DumpROM.app"
OUT = "install/AllApps-B.pack.new"

def align4(n): return (n + 3) & ~3

def main():
    data = Path(DEV_PACK).read_bytes()
    _, apps = parse_pack(data)
    blobs = []
    for a in apps:
        b = data[a["offset"]:a["offset"] + a["bundle_len"]]
        blobs.append(b)
    new = Path(NEW_BUNDLE).read_bytes()
    # sanity: the bundle must be well-formed and its len aligned by us at pack time
    blobs.append(new)

    n = len(blobs)
    head = align4(12 + 4 * n)
    offs, blob, cur = [], bytearray(), head
    for b in blobs:
        offs.append(cur)
        blob += b
        blob += b"\0" * (align4(len(b)) - len(b))
        cur += align4(len(b))
    out = bytearray(head) + blob
    struct.pack_into("<III", out, 0, 0x4b504248, 1, n)   # HBPK v1
    for i, o in enumerate(offs):
        struct.pack_into("<I", out, 12 + 4 * i, o)
    Path(OUT).write_bytes(out)

    # verify: re-parse, old 8 byte-identical + in order, DumpROM last
    _, chk = parse_pack(bytes(out))
    names = [a["name"] for a in chk]
    assert names[:8] == [a["name"] for a in apps], names
    assert names[8] == "DumpROM" and chk[8]["sbid"] == "hb.dumprom"
    for i in range(8):
        o = chk[i]["offset"]
        assert bytes(out)[o:o + chk[i]["bundle_len"]] == blobs[i], f"bundle {i} changed"
    ids = [a["app_id"] for a in chk]
    assert len(set(ids)) == len(ids), "app_id collision"
    print(f"OK: {OUT} — {n} apps, {len(out)} bytes; order: {names}")
    print(f"    DumpROM app_id=0x{chk[8]['app_id']:08x} kind={chk[8]['kind']}")

if __name__ == "__main__":
    main()
