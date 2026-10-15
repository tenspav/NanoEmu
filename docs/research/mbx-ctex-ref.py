# ctref.py V PAM -- gltest 78-87 reference (compressed / paletted textures)
# vs the GPU frame dump: rebuilds the app's LCG data, decodes PVRTC with
# texture2ddecoder (independent, AssetStudio's port of Imagination's decoder)
# and expands palettes per GL_OES_compressed_paletted_texture.
import sys, texture2ddecoder as t2d
V, fn = int(sys.argv[1]), sys.argv[2]
FMT = {78: ('pvr', 4, True), 79: ('pvr', 2, True), 80: ('pvr', 4, False),
       81: ('pvr', 2, False), 82: ('rgba8', 4), 83: ('rgba8', 8),
       84: ('565', 4), 85: ('rgba4', 8), 86: ('rgb8', 8), 87: ('5551', 4)}
PB = {'rgba8': 4, 'rgb8': 3, '565': 2, 'rgba4': 2, '5551': 2}
T = 64
f = FMT[V]
lcg = 1
def rnd():
    global lcg
    lcg = (lcg * 1103515245 + 12345) & 0xffffffff
    return (lcg >> 16) & 0xff
tex = []                                    # RGBA per texel, row-major
if f[0] == 'pvr':
    data = bytes(rnd() for _ in range(T * T * f[1] // 8))
    bgra = t2d.decode_pvrtc(data, T, T, f[1] == 2)
    for i in range(T * T):
        b, g, r, a = bgra[4 * i:4 * i + 4]
        tex.append((r, g, b, a if f[2] else 255))
else:
    n = 1 << f[1]
    pal = bytes(rnd() for _ in range(n * PB[f[0]]))
    def entry(k):
        p = pal[k * PB[f[0]]:(k + 1) * PB[f[0]]]
        if f[0] == 'rgba8': return tuple(p)
        if f[0] == 'rgb8': return (p[0], p[1], p[2], 255)
        v = p[0] | p[1] << 8                # 16-bit entries little-endian
        if f[0] == '565':
            r, g, b = v >> 11, (v >> 5) & 63, v & 31
            return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255)
        if f[0] == 'rgba4':
            c = [(v >> s) & 15 for s in (12, 8, 4, 0)]
            return tuple(x * 17 for x in c)
        r, g, b, a = v >> 11, (v >> 6) & 31, (v >> 1) & 31, v & 1
        return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255 * a)
    for y in range(T):
        for x in range(T):
            tex.append(entry((x * 7 + y * 3 + (x * y >> 3)) & (n - 1)))
hdr, body = open(fn, 'rb').read().split(b'ENDHDR\n', 1)
W = int(hdr.split(b'WIDTH ')[1].split()[0])
bad = tot = 0; worst = 0; ex = []
for y in range(24, 216):
    for x in range(24, 216):
        exp = tex[((y - 24) // 3) * T + (x - 24) // 3]
        got = tuple(body[4 * (y * W + x):4 * (y * W + x) + 4])
        d = max(abs(a - b) for a, b in zip(exp, got))
        tot += 1; worst = max(worst, d)
        if d > 1:
            bad += 1
            if len(ex) < 4: ex.append(((x, y), exp, got))
print(f"v{V}: {tot - bad}/{tot} pixels within 1, max diff {worst}", ex)
