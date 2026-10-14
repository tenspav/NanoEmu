#!/usr/bin/env python3
"""rsp_trace.py -- tiny GDB remote-protocol client for QEMU's gdbstub (-gdb).

  rsp_trace.py HOST:PORT ADDR[,ADDR...] [SECONDS]
Sets Thumb breakpoints at each ADDR; on a hit prints r0-r3, sp, lr, pc,
then breaks once at the return address and prints r0 (the return value).
Runs until SECONDS elapse (default 120), then detaches."""
import os, socket, sys, time

# RSP_STACK=lo:hi -> on every hit also list Thumb return addresses found in
# that memory range (another thread's stack, e.g. a blocked requester)
STACK = [int(x, 0) for x in os.environ['RSP_STACK'].split(':')] \
    if os.environ.get('RSP_STACK') else None
# RSP_SPSTACK=N -> on every hit also list Thumb return addresses in the N
# bytes above the stopped thread's sp
SPSTACK = int(os.environ.get('RSP_SPSTACK', '0'), 0)


class RSP:
    def __init__(self, hp):
        h, p = hp.rsplit(':', 1)
        self.s = socket.create_connection((h, int(p)))
        self.buf = b''

    def send(self, pkt):
        data = pkt.encode()
        self.s.sendall(b'$' + data + b'#%02x' % (sum(data) & 0xff))
        self._ack()

    def _ack(self):
        while True:
            c = self._byte()
            if c == b'+':
                return

    def _byte(self):
        if not self.buf:
            self.buf = self.s.recv(65536)
            if not self.buf:
                raise EOFError
        c, self.buf = self.buf[:1], self.buf[1:]
        return c

    def recv(self, timeout=None):
        self.s.settimeout(timeout)
        try:
            while self._byte() != b'$':
                pass
        except socket.timeout:
            return None
        finally:
            self.s.settimeout(None)
        d = b''
        while True:
            c = self._byte()
            if c == b'#':
                self._byte(); self._byte()
                self.s.sendall(b'+')
                return d.decode()
            d += c

    def cmd(self, pkt):
        self.send(pkt)
        return self.recv()

    def regs(self):
        g = self.cmd('g')
        return [int.from_bytes(bytes.fromhex(g[i * 8:i * 8 + 8]), 'little')
                for i in range(16)]


def main():
    r = RSP(sys.argv[1])
    addrs = [int(a, 0) & ~1 for a in sys.argv[2].split(',')]
    end = time.time() + (float(sys.argv[3]) if len(sys.argv) > 3 else 120)
    r.cmd('?')
    for a in addrs:
        print('Z0 0x%08x ->' % a, r.cmd('Z0,%x,2' % a))
    pending = {}                        # return addr -> label
    args = {}                           # return addr -> r1 at the call
    r.send('c')
    while time.time() < end:
        stop = r.recv(timeout=1)
        if stop is None:
            continue
        g = r.regs()
        pc = g[15]
        if pc in pending:
            print('  ret to 0x%08x from %s: r0=0x%08x r1=0x%08x' %
                  (pc, pending.pop(pc), g[0], g[1]))
            ptrs = args.pop(pc, None)
            for reg, buf in zip(('r1', 'r2'), ptrs or ()):
                if 0x08000000 <= buf < 0x0c000000:  # 32 bytes at call's r1/r2
                    print('    [%s 0x%08x] %s' % (reg, buf, r.cmd('m%x,20' % buf)))
            if pc not in addrs:
                r.cmd('z0,%x,2' % pc)
                sys.stdout.flush()
                r.send('c')
                continue
        else:
            print('hit 0x%08x r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x' %
                  (pc, g[0], g[1], g[2], g[3], g[13], g[14]))
            if 0x08000000 <= g[0] < 0x0c000000:     # 16 bytes at r0
                print('    [r0 0x%08x] %s' % (g[0], r.cmd('m%x,10' % g[0])))
            if STACK or SPSTACK:
                lo, hi = STACK if STACK else (g[13], g[13] + SPSTACK)
                mem = bytes.fromhex(r.cmd('m%x,%x' % (lo, hi - lo)))
                rets = []
                for i in range(0, len(mem) - 3, 4):
                    v = int.from_bytes(mem[i:i + 4], 'little')
                    if v & 1 and 0x08000000 <= v < 0x0a000000:
                        rets.append('%08x@%x' % (v, lo + i))
                print('    stack rets:', ' '.join(rets))
            lr = g[14] & ~1
            if lr not in pending:
                pending[lr] = '0x%08x' % pc
                args[lr] = (g[1], g[2])
                r.cmd('Z0,%x,2' % lr)
        sys.stdout.flush()
        # step over the breakpoint we are stopped on (gdbstub won't)
        if pc in addrs or pc in pending:
            r.cmd('z0,%x,2' % pc)
            r.cmd('s')
            r.cmd('Z0,%x,2' % pc)
        r.send('c')
    r.s.sendall(b'\x03')
    r.recv(timeout=2)
    for a in addrs:
        r.cmd('z0,%x,2' % a)
    r.send('D')
    print('detached')


main()
