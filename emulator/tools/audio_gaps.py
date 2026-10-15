#!/usr/bin/env python3
"""audio_gaps.py HOST.wav [REF.raw] -- find dropouts in what the host audio
backend played (QEMU -audiodev wav,path=HOST.wav): runs of >= 5 ms of
digital silence inside the music. REF (run.sh --audio-out, the raw guest
stream) gives the music's length for comparison. Stock Python only."""
import array, sys, wave

w = wave.open(sys.argv[1])
rate, ch = w.getframerate(), w.getnchannels()
pcm = array.array('h', w.readframes(w.getnframes()))
frames = len(pcm) // ch
loud = [any(pcm[i * ch + c] for c in range(ch)) for i in range(frames)]
first = loud.index(True) if True in loud else None
last = frames - 1 - loud[::-1].index(True) if first is not None else None
print('host: %.2f s at %d Hz, music %.2f-%.2f s' % (
    frames / rate, rate, (first or 0) / rate, (last or 0) / rate))
gaps, run = [], 0
for i in range(first or 0, (last or 0) + 1):
    if loud[i]:
        if run >= rate * 5 // 1000:
            gaps.append((i - run, run))
        run = 0
    else:
        run += 1
print('dropouts >= 5 ms inside the music: %d' % len(gaps))
for g in gaps[:10]:
    print('  at %.3f s: %.1f ms' % (g[0] / rate, g[1] * 1000 / rate))
if len(sys.argv) > 2:
    ref = array.array('h', open(sys.argv[2], 'rb').read())
    nz = [i for i in range(0, len(ref), 2) if ref[i] or ref[i + 1]]
    if nz:
        print('guest stream: music %.2f s' % ((nz[-1] - nz[0]) / 2 / 44100))
