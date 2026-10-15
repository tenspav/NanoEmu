#!/usr/bin/env python3
"""bt_hci_host.py -- host end of the nano7 Bluetooth UART socket (run.sh --bt-hci-socket).

  bt_hci_host.py SOCKET|HOST:PORT [--log FILE]

Connects to the emulator's listening socket, frames the raw HCI H4 stream and
answers like a minimal controller: Command Complete (status 0, plausible return
parameters) for every command, Command Status for Inquiry / Create_Connection
(+ empty Inquiry Complete / page timeout). It is the starting point for a real
bridge (a host adapter via a raw HCI socket, BlueZ /dev/vhci, or HID report
injection), and proves the socket path: the OS must reach "Bluetooth: On".
"""
import socket
import struct
import sys
import time

BDADDR = bytes([0x01, 0x00, 0x00, 0x37, 0x4e, 0x02])
FEATURES = bytes([0xbf, 0xfe, 0xcf, 0xfe, 0xdb, 0xff, 0x7b, 0x87])

RET = {                                    # opcode -> return params after status
    0x1001: bytes([6, 0x00, 0x10, 6, 0x0f, 0x00, 0x0b, 0x41]),
    0x1002: b'\xff' * 64,
    0x1003: FEATURES,
    0x1005: struct.pack('<HBHH', 1021, 64, 8, 0),
    0x1009: BDADDR,
    0x0c14: b'iPod'.ljust(248, b'\0'),
    0x0c19: b'\0',
    0x0c23: b'\x14\x04\x20',
    0x2002: struct.pack('<HB', 27, 8),
    0x2003: b'\x01' + b'\0' * 7,
    0x2007: b'\x04',
    0x200f: b'\x08',
    0x201c: b'\xff' * 5 + b'\0' * 3,
}


def event(code, par):
    return bytes([4, code, len(par)]) + par


def handle_cmd(op, par, out, state):
    if op == 0x0401:                       # Inquiry: nobody around
        out(event(0x0f, bytes([0, 1, op & 0xff, op >> 8])))
        out(event(0x01, b'\0'))
        return
    if op == 0x0405:                       # Create_Connection: page timeout
        out(event(0x0f, bytes([0, 1, op & 0xff, op >> 8])))
        out(event(0x03, bytes([4, 0, 0]) + par[:6].ljust(6, b'\0') + b'\x01\x00'))
        return
    if op == 0xfc01 and len(par) >= 6:     # host writes the device address
        state['bdaddr'] = par[:6]
    ret = RET.get(op, b'')
    if op == 0x1009:
        ret = state.get('bdaddr', BDADDR)
    out(event(0x0e, bytes([1, op & 0xff, op >> 8, 0]) + ret))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    target = sys.argv[1]
    log = open(sys.argv[sys.argv.index('--log') + 1], 'w') if '--log' in sys.argv else sys.stdout
    while True:                            # the emulator may not be up yet
        try:
            if ':' in target and not target.startswith('/'):
                h, p = target.rsplit(':', 1)
                s = socket.create_connection((h or '127.0.0.1', int(p)))
            else:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(target)
            break
        except OSError:
            time.sleep(0.5)
    print('connected to', target, file=log, flush=True)
    t0 = time.time()
    buf = b''
    state = {}
    stats = {'cmd': 0, 'evt': 0}

    def out(pkt):
        stats['evt'] += 1
        print('%8.3f < %s' % (time.time() - t0, pkt[:16].hex(' ')), file=log, flush=True)
        s.sendall(pkt)

    while True:
        d = s.recv(65536)
        if not d:
            break
        buf += d
        while buf:
            t = buf[0]
            if t == 1 and len(buf) >= 4 and len(buf) >= 4 + buf[3]:
                n = 4 + buf[3]
                op = buf[1] | buf[2] << 8
                stats['cmd'] += 1
                print('%8.3f > cmd 0x%04x %s' % (time.time() - t0, op, buf[4:n][:12].hex(' ')),
                      file=log, flush=True)
                handle_cmd(op, buf[4:n], out, state)
            elif t == 2 and len(buf) >= 5 and len(buf) >= 5 + (buf[3] | buf[4] << 8):
                n = 5 + (buf[3] | buf[4] << 8)
                print('%8.3f > acl %s' % (time.time() - t0, buf[:n][:16].hex(' ')), file=log, flush=True)
            elif t in (1, 2):
                break                      # incomplete
            else:
                print('stray byte 0x%02x' % t, file=log, flush=True)
                n = 1
            buf = buf[n:]
    print('closed: %d commands, %d events' % (stats['cmd'], stats['evt']), file=log, flush=True)


if __name__ == '__main__':
    main()
