#!/usr/bin/env python3
"""Parse an HBPK app pack (NanoApps tools/mkapp.py format) and list its apps."""
import struct, sys

HDR_FMT = "<IHHIIIIIIIIIII"
HDR_SIZE = struct.calcsize(HDR_FMT)

def parse_bundle(blob):
    (magic, ver, kind, app_id, label_id, screen_id, layout_id, os_handler,
     label_tag, screen_tag, sbid_off, name_off, bundle_len, nsec) = \
        struct.unpack_from(HDR_FMT, blob, 0)
    assert magic == 0x50414248, f"bad bundle magic 0x{magic:08x}"
    sbid = blob[sbid_off:blob.index(b"\0", sbid_off)].decode()
    name = blob[name_off:blob.index(b"\0", name_off)].decode()
    secs = []
    for i in range(nsec):
        kind_s, off, ln = struct.unpack_from("<III", blob, HDR_SIZE + i * 12)
        secs.append((kind_s, off, ln))
    return dict(ver=ver, kind=kind, app_id=app_id, label_id=label_id,
                screen_id=screen_id, layout_id=layout_id, os_handler=os_handler,
                label_tag=label_tag, screen_tag=screen_tag, sbid=sbid, name=name,
                bundle_len=bundle_len, sections=secs)

def parse_pack(data):
    magic, ver, n = struct.unpack_from("<III", data, 0)
    assert magic == 0x4b504248, f"bad pack magic 0x{magic:08x}"
    offs = struct.unpack_from(f"<{n}I", data, 12)
    out = []
    for o in offs:
        b = parse_bundle(data[o:])
        b["offset"] = o
        out.append(b)
    return ver, out

if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    ver, apps = parse_pack(data)
    print(f"{sys.argv[1]}: version={ver} apps={len(apps)} bytes={len(data)}")
    for i, a in enumerate(apps):
        print(f"  [{i}] name={a['name']!r} sbid={a['sbid']!r} kind={a['kind']} "
              f"app_id=0x{a['app_id']:08x} label_id=0x{a['label_id']:08x} "
              f"screen_id={a['screen_id']} layout_id={a['layout_id']} "
              f"label_tag=0x{a['label_tag']:08x} screen_tag=0x{a['screen_tag']:08x} "
              f"len={a['bundle_len']} secs={[(s[0], s[2]) for s in a['sections']]}")
