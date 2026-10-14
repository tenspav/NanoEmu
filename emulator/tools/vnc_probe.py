#!/usr/bin/env python3
"""vnc_probe.py -- minimal RFB 3.8 client (no auth) to test the nano7 display.

  vnc_probe.py HOST:PORT shot OUT.png            grab one full frame
  vnc_probe.py HOST:PORT tap X Y [OUTPREFIX]     press+release at (X, Y), frames +1/3/8 s
  vnc_probe.py HOST:PORT key KEYSYM              press+release a key (e.g. 0x68 = h)
  vnc_probe.py HOST:PORT hold KEYSYM SECONDS     hold a key down (e.g. 0xff09 = Tab)
  vnc_probe.py HOST:PORT watch SECONDS           count frame updates (proof of a live stream)
Coordinates are display pixels (240x432)."""
import socket, struct, sys, time, zlib


def rx(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b


def connect(hp):
    host, port = hp.rsplit(':', 1)
    s = socket.create_connection((host, int(port)))
    rx(s, 12)
    s.sendall(b'RFB 003.008\n')
    n = rx(s, 1)[0]
    types = rx(s, n)
    assert 1 in types, types
    s.sendall(b'\x01')
    assert struct.unpack('>I', rx(s, 4))[0] == 0
    s.sendall(b'\x01')                                    # shared
    w, h = struct.unpack('>HH', rx(s, 4))
    rx(s, 16)
    rx(s, struct.unpack('>I', rx(s, 4))[0])               # name
    # 32 bpp, depth 24, little endian, true colour, r/g/b max 255 shift 16/8/0
    s.sendall(struct.pack('>BxxxBBBBHHHBBBxxx', 0, 32, 24, 0, 1, 255, 255, 255,
                          16, 8, 0))
    s.sendall(struct.pack('>BxHi', 2, 1, 0))              # encodings: raw
    return s, w, h


def request(s, w, h, incremental):
    s.sendall(struct.pack('>BBHHHH', 3, incremental, 0, 0, w, h))


def receive(s, w, h, fb):
    t, _, n = struct.unpack('>BBH', rx(s, 4))
    while t != 0:                       # skip bell / cut text / colour maps
        if t == 3:
            rx(s, 3)
            rx(s, struct.unpack('>I', rx(s, 4))[0])
        t, _, n = struct.unpack('>BBH', rx(s, 4))
    for _ in range(n):
        x, y, rw, rh, enc = struct.unpack('>HHHHi', rx(s, 12))
        if enc == -223:                 # desktop size
            continue
        assert enc == 0, enc
        data = rx(s, rw * rh * 4)
        for j in range(rh):
            o = ((y + j) * w + x) * 4
            fb[o:o + rw * 4] = data[j * rw * 4:(j + 1) * rw * 4]
    return n


def update(s, w, h, fb, incremental):
    request(s, w, h, incremental)
    return receive(s, w, h, fb)


def png(path, w, h, fb):
    raw = b''.join(b'\0' + bytes(fb[(y * w + x) * 4 + c] for x in range(w)
                                 for c in (2, 1, 0)) for y in range(h))
    chunk = lambda t, b: (struct.pack('>I', len(b)) + t + b +
                          struct.pack('>I', zlib.crc32(t + b)))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' +
                           chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def pointer(s, mask, x, y):
    s.sendall(struct.pack('>BBHH', 5, mask, x, y))


def main():
    s, w, h = connect(sys.argv[1])
    fb = bytearray(w * h * 4)
    cmd = sys.argv[2]
    update(s, w, h, fb, 0)
    if cmd == 'shot':
        png(sys.argv[3], w, h, fb)
        print('frame %dx%d -> %s' % (w, h, sys.argv[3]))
    elif cmd == 'tap':
        x, y = int(sys.argv[3]), int(sys.argv[4])
        pointer(s, 0, x, y)
        time.sleep(0.05)
        pointer(s, 1, x, y)
        time.sleep(0.15)
        pointer(s, 0, x, y)
        print('tap at (%d, %d)' % (x, y))
        if len(sys.argv) > 5:           # OUT prefix: frames 1, 3, 8 s later
            for t, d in ((1, 1), (3, 2), (8, 5)):
                time.sleep(d)
                update(s, w, h, fb, 0)
                png('%s-%ds.png' % (sys.argv[5], t), w, h, fb)
                print('after-tap frame +%ds -> %s-%ds.png' % (t, sys.argv[5], t))
    elif cmd == 'key':
        k = int(sys.argv[3], 0)
        s.sendall(struct.pack('>BBxxI', 4, 1, k))
        time.sleep(0.1)
        s.sendall(struct.pack('>BBxxI', 4, 0, k))
        print('key 0x%x' % k)
    elif cmd == 'hold':
        k = int(sys.argv[3], 0)
        s.sendall(struct.pack('>BBxxI', 4, 1, k))
        time.sleep(float(sys.argv[4]))
        s.sendall(struct.pack('>BBxxI', 4, 0, k))
        print('held key 0x%x for %s s' % (k, sys.argv[4]))
    elif cmd == 'watch':                # updates arrive only when the screen changes
        end, rects, ups = time.time() + float(sys.argv[3]), 0, 0
        s.settimeout(0.2)
        request(s, w, h, 1)
        while time.time() < end:
            try:
                rects += receive(s, w, h, fb)
            except socket.timeout:
                continue                # request still pending: screen quiet
            ups += 1
            request(s, w, h, 1)
        print('%d frame updates, %d rects in %s s' % (ups, rects, sys.argv[3]))


main()
