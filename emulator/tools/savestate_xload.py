#!/usr/bin/env python3
"""Load a save-state slot into a fresh emulator process, compare with the saved frame, check animation.
usage: savestate_xload.py OUTDIR SLOTDIR REF.png  (RUNSH=path overrides emulator/run.sh)"""
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
slot, ref = Path(sys.argv[2]).resolve(), Image.open(sys.argv[3]).convert('RGB')
import shutil; shutil.copytree(slot, state / 'slot0')
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


q('qmp_capabilities'); time.sleep(3)
key('f5'); time.sleep(1); key('f4'); time.sleep(2)
L = shot('L_loaded'); print('fresh-process load vs saved frame differing px:', diff(L, ref))
key('f5'); time.sleep(0.5)
fr = [shot(f'run{i}') for i in range(6) if not time.sleep(0.3)]
print('after resume distinct frames', len({h(i) for i in fr}), 'changed vs saved', [diff(i, ref) > 0 for i in fr])
q('quit'); p.wait(timeout=20)
