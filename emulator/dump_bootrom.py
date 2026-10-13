#!/usr/bin/env python3
"""Dump iPod nano 7G VROM/bootrom via SCSI peek. Saves progress, resumable."""
import os, subprocess, sys, time

DEV = "/dev/sdb"
START = 0x20000000
SIZE = 0x40000  # 256 KB
OUT = sys.argv[1] if len(sys.argv) > 1 else "bootrom-vrom.bin"

def peek512(addr):
    ab = [f"{(addr >> s) & 0xff:02x}" for s in (24, 16, 8, 0)]
    for _ in range(10):
        subprocess.run(["sg_raw", "-o", "/tmp/vrom.bin", "-r", "512",
                        DEV, "c6", "96", "02", *ab],
                       capture_output=True)
        try:
            data = open("/tmp/vrom.bin", "rb").read()
        except FileNotFoundError:
            data = b""
        if any(data):
            return data
        time.sleep(0.3)
    return bytes(512)

done = os.path.getsize(OUT) if os.path.exists(OUT) else 0
done -= done % 0x200
print(f"resuming at {done:#x}", flush=True)

with open(OUT, "r+b" if done else "wb") as f:
    for off in range(done, SIZE, 0x200):
        f.seek(off)
        f.write(peek512(START + off))
        f.flush()
        if off % 0x2000 == 0:
            print(f"{off:#x}/{SIZE:#x}", flush=True)

print("done:", OUT)
