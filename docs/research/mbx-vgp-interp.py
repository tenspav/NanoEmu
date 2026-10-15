import json, struct, math, sys, glob
def f32(w): return struct.unpack('<f', struct.pack('<I', w & 0xffffffff))[0]
def sw(v, s): return [v[(s >> (2*i)) & 3] for i in range(4)]
class VGP:
    def __init__(s, prog, stage, cst):
        s.prog, s.stage, s.cst = prog, stage, cst
    def inputs(s, fmt, words, ubyte_order):
        v = [[0.0, 0.0, 0.0, 1.0] for _ in range(8)]; off = 0
        for k in range(8):
            nib = (fmt >> (4*k)) & 0xf
            if nib == 0: continue
            if nib == 5:
                w = words[off]; b = [(w >> (8*i)) & 0xff for i in range(4)]
                v[k] = [b[i]/255.0 for i in ubyte_order]; off += 1
            elif nib == 7:
                w = words[off]; v[k] = [float(w & 0xffff), float(w >> 16), 0.0, 1.0]; off += 1
            elif nib == 8:
                w0, w1 = words[off], words[off+1]
                q = [w0 & 0xffff, w0 >> 16, w1 & 0xffff, w1 >> 16]
                v[k] = [float(x - 65536 if x & 0x8000 else x) for x in q]; off += 2
            else:
                for i in range(nib): v[k][i] = f32(words[off+i])
                off += nib
        return v
    def run(s, ctl, vin, trace=False):
        r = [[0.0]*4 for _ in range(48)]; o = [[0.0, 0.0, 0.0, 1.0] for _ in range(8)]; a0 = 0
        for bit in range(16):
            if not (ctl >> bit) & 1: continue
            d = s.stage[bit]; st, en = d & 0xff, (d >> 8) & 0xff
            base = ((d >> 20) & 0xf) * 16
            loops = ((d >> 16) & 0xf) + 1 if bit in (8, 9) else 1
            for _ in range(loops):
                for pc in range(st, en + 1):
                    A, B = s.prog[pc]
                    op = A & 0x1f
                    src = []
                    for k in range(3):
                        f = (B >> (16 + 2*k)) & 3; idx = (B >> (4*k)) & 0xf
                        if f == 0: val = vin[idx]
                        elif f == 1:
                            raw = ((B >> 12) & 0xf) << 4 | idx
                            ci = base + (raw & 0x7f) + (a0 if raw & 0x80 else 0)
                            val = s.cst[ci & 0xff]
                        elif f == 2: val = r[idx]
                        else: val = [0.0]*4
                        val = sw(val, (A >> (8 + 8*k)) & 0xff)
                        if (A >> (5 + k)) & 1: val = [-x for x in val]
                        src.append(val)
                    s0, s1, s2 = src
                    if op == 1: res = s0
                    elif op == 2: res = [s0[i]+s1[i] for i in range(4)]
                    elif op == 3: res = [s0[i]*s1[i]+s2[i] for i in range(4)]
                    elif op == 4: res = [s0[i]*s1[i] for i in range(4)]
                    elif op == 5: x = s0[3]; res = [1.0/x if x != 0 else float('inf')]*4
                    elif op == 6: x = abs(s0[3]); res = [1.0/math.sqrt(x) if x > 0 else float('inf')]*4
                    elif op == 7: res = [sum(s0[i]*s1[i] for i in range(3))]*4
                    elif op == 8: res = [sum(s0[i]*s1[i] for i in range(4))]*4
                    elif op == 9: res = [min(s0[i], s1[i]) for i in range(4)]
                    elif op == 0xa: res = [max(s0[i], s1[i]) for i in range(4)]
                    elif op == 0xc: res = [1.0 if s0[i] >= s1[i] else 0.0 for i in range(4)]
                    elif op == 0xf:
                        x, y, w = s0[0], s0[1], s0[3]
                        spec = (max(y, 0.0) ** w if y > 0 or w != 0 else 1.0) if x > 0 else 0.0
                        res = [1.0, max(x, 0.0), spec, 1.0]
                    elif op == 0x10: res = [1.0, s0[1]*s1[1], s0[2], s1[3]]
                    elif op == 0x12: a0 = int(math.floor(s0[0])); res = None
                    elif op == 0x13: res = [2.0 ** s0[3]]*4
                    else: raise Exception('op %x at %x' % (op, pc))
                    if res is None: continue
                    dst = B >> 26; mask = (B >> 22) & 0xf
                    tgt = r[dst - 16] if dst >= 16 else o[dst]
                    for i in range(4):
                        if mask >> i & 1: tgt[i] = res[i]
                    if trace: print('  %02x op%02x dst %d mask %x -> %s' % (pc, op, dst, mask, [round(x,4) for x in res]))
        return o
def load(fn):
    import re
    fix=lambda l: re.sub(r'(?<![A-Za-z])-?inf', lambda m: '-Infinity' if m.group(0).startswith('-') else 'Infinity', re.sub(r'-?nan', 'NaN', l))
    return [json.loads(fix(l)) for l in open(fn)]
if __name__ == '__main__':
    for fn in sorted(glob.glob('vp/*.jsonl')):
        for d in load(fn):
            vg = VGP(d['prog'], d['stage'], d['cst'])
            words = d['vtx']; worst = {}
            for order in ((0,1,2,3),):
                for vi, (raw, hle) in enumerate(zip(words, d['out'])):
                    vin = vg.inputs(d['fmt'], raw, order)
                    o = vg.run(d['ctl'], vin)
                    cmp = {'p': (o[0], hle['p']), 'uv': (o[3][:2], hle['uv']), 'uv1': (o[4][:2], hle['uv1'])}
                    for k, (a, b) in cmp.items():
                        e = max(abs(x - y) for x, y in zip(a, b)); worst[k] = max(worst.get(k, 0), e)
                    col = [o[1][i] for i in range(4)]
                    worst['col'] = max(worst.get('col', 0), max(abs(x - y) for x, y in zip(col, hle['c'])))
                    worst['fog'] = max(worst.get('fog', 0), abs(min(max(o[2][3], 0), 1) - hle['f']))
            print('%-14s ctl %08x fmt %04x  ' % (fn, d['ctl'], d['fmt']) + '  '.join('%s %.4g' % kv for kv in worst.items()))
