#!/usr/bin/env python3
"""Direct SG_IO test of the custom SCSI peek (c6 96 02), bypassing sg_raw."""
import ctypes, fcntl, os, sys

SG_IO = 0x2285
SG_DXFER_FROM_DEV = -3

class SgIoHdr(ctypes.Structure):
    _fields_ = [("interface_id", ctypes.c_int), ("dxfer_direction", ctypes.c_int),
                ("cmd_len", ctypes.c_ubyte), ("mx_sb_len", ctypes.c_ubyte),
                ("iovec_count", ctypes.c_ushort), ("dxfer_len", ctypes.c_uint),
                ("dxferp", ctypes.c_void_p), ("cmdp", ctypes.c_void_p),
                ("sbp", ctypes.c_void_p), ("timeout", ctypes.c_uint),
                ("flags", ctypes.c_uint), ("pack_id", ctypes.c_int),
                ("usr_ptr", ctypes.c_void_p), ("status", ctypes.c_ubyte),
                ("masked_status", ctypes.c_ubyte), ("msg_status", ctypes.c_ubyte),
                ("sb_len_wr", ctypes.c_ubyte), ("host_status", ctypes.c_ushort),
                ("driver_status", ctypes.c_ushort), ("resid", ctypes.c_int),
                ("duration", ctypes.c_uint), ("info", ctypes.c_uint)]

dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/sdb"
addr = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x08000000

cdb = bytes([0xC6, 0x96, 0x02,
             (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
             (addr >> 8) & 0xFF, addr & 0xFF])
cdb_buf = ctypes.create_string_buffer(cdb, len(cdb))
data_buf = ctypes.create_string_buffer(512)
sense_buf = ctypes.create_string_buffer(64)

h = SgIoHdr(interface_id=ord('S'), dxfer_direction=SG_DXFER_FROM_DEV,
            cmd_len=len(cdb), mx_sb_len=64, dxfer_len=512,
            dxferp=ctypes.addressof(data_buf), cmdp=ctypes.addressof(cdb_buf),
            sbp=ctypes.addressof(sense_buf), timeout=10000)

fd = os.open(dev, os.O_RDWR)
r = fcntl.ioctl(fd, SG_IO, h)
os.close(fd)
print("ioctl:", r, "status:", h.status, "host:", h.host_status,
      "driver:", hex(h.driver_status), "resid:", h.resid)
print("sense:", sense_buf.raw[:h.sb_len_wr].hex())
d = data_buf.raw
print("data[:32]:", d[:32].hex())
print("nonzero:", any(d))
open("/tmp/sgio_out.bin", "wb").write(d)
