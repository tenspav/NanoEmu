#!/usr/bin/env python3
"""Save-state round trip on a GL homebrew (gltest in the Blocks slot); RUNSH=path overrides emulator/run.sh.
usage: savestate_roundtrip.py OUTDIR [extra run.sh args]"""
import json, subprocess, sys, time, hashlib
from pathlib import Path
from PIL import Image, ImageChops

ROOT = Path('/home/pavliukov/ipodwork/NanoRework')
out = Path(sys.argv[1]).resolve(); out.mkdir(parents=True, exist_ok=True)
state = out / 'state'
import os
cmd = (os.environ.get('RUNSH', 'emulator/run.sh') + ' ' + '--ramfull dumps/snap6/ram-full.bin --iram dumps/snap6/iram.bin '
       '--ctx emulator/fixups/snap6-ctx.bin --overlay dumps/snap6/overlay.bin '
       '--os-poke-file emulator/fixups/snap6-resume.poke --torn-map dumps/snap6/torn-map.bin '
       '--nand-miss oracle --nand-disk dumps/ipod-disk.dd --nand-disk-offset 49216 '
       '--nand-rsrc emulator/rsrc/rsrc-1.1.2.img1 '
       '--nand-patch emulator/rsrc/gltest-as-blocks.patch '
       '-display none -qmp stdio '
       f'-M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100,state-dir={state}').split()
cmd += sys.argv[2:]
log = open(out / 'emu.log', 'w')
p = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=log, text=True)
p.stdout.readline()


def q(ex, args={}):
    p.stdin.write(json.dumps(dict(execute=ex, arguments=args)) + '\n'); p.stdin.flush()
    while True:
        r = json.loads(p.stdout.readline())
        if 'return' in r or 'error' in r:
            if 'error' in r:
                print('QMP error', ex, r)
            return r


def key(k):
    q('human-monitor-command', {'command-line': f'sendkey {k}'})


def touch(x, y, down=None):
    es = [dict(type='abs', data=dict(axis='x', value=x)),
          dict(type='abs', data=dict(axis='y', value=y))]
    if down is not None:
        es.append(dict(type='btn', data=dict(down=down, button='left')))
    q('input-send-event', dict(events=es)); time.sleep(.13)


def shot(name):
    ppm = out / (name + '.ppm')
    q('screendump', {'filename': str(ppm)})
    time.sleep(0.3)
    im = Image.open(ppm).convert('RGB'); im.save(out / (name + '.png')); ppm.unlink()
    return im


def h(im):
    return hashlib.md5(im.tobytes()).hexdigest()[:12]


def diff(a, b):
    d = ImageChops.difference(a, b).getbbox()
    if not d:
        return 0
    return sum(1 for x, y in zip(a.getdata(), b.getdata()) if x != y)


q('qmp_capabilities'); time.sleep(3); B0 = shot('B0_boot'); key('h'); time.sleep(2)
for x, d in [(4000, True), (9000, None), (14000, None), (19000, None), (24000, None), (29000, False)]:
    touch(x, 16000, d)
time.sleep(3); touch(8700, 17500, True); touch(8700, 17500, False)
time.sleep(8)
pre = [shot(f'pre{i}') for i in range(6) if not time.sleep(0.25)]
print('pre frames', [h(i) for i in pre], flush=True)
key('f5'); time.sleep(1)                      # soft pause
A = shot('A_saved'); key('f2'); time.sleep(3)
A2 = shot('A_after_save')
import shutil; shutil.copytree(state / 'slot0', out / 'slotA')
key('f5'); time.sleep(1)                      # resume
key('h'); time.sleep(4)                       # leave the app: home screen
C = shot('C_modified')
key('f5'); time.sleep(1)
key('f4'); time.sleep(3)                      # load while paused
D = shot('D_loaded')
print('A', h(A), 'A2', h(A2), 'C', h(C), 'D', h(D))
print('C vs A differing px:', diff(A, C), '| D vs A differing px:', diff(A, D), flush=True)
key('f5'); time.sleep(1)                      # resume after load
post = []
for i in range(12):
    post.append(shot(f'post{i}')); time.sleep(0.25)
print('post frames', [h(i) for i in post])
print('post0 vs A differing px:', diff(post[0], A))
print('post distinct', len({h(i) for i in post}), 'post vs pre exact matches',
      sum(1 for i in post if h(i) in {h(j) for j in pre}))
# touch held across a save: the load must release it
touch(16000, 16000, True); time.sleep(0.5); key('f2'); time.sleep(2)
touch(16000, 16000, False); time.sleep(1); key('f4'); time.sleep(2)
# QMP system_reset = reload the boot state (F1)
key('f5'); time.sleep(1)                      # pause: compare the exact boot frame
q('system_reset'); time.sleep(3)
R = shot('R_reset')
print('reset vs boot frame differing px:', diff(R, B0))
key('f5'); time.sleep(3)
R2 = shot('R_reset_running')
q('quit'); p.wait(timeout=20)
