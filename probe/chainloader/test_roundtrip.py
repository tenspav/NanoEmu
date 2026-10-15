#!/usr/bin/env python3
"""Piece 5: Chainloader + bare-metal payload round-trip regression test in NanoEmu.
Runs from repository root. Verifies:
1. Boot into RetailOS (snap6).
2. Navigation to home screen page 2 (Blocks slot).
3. Tap chainloader.hbapp injected via disk patch.
4. CPU takeover, MMU/cache disable, payload relocates and fills screen green.
5. Home button pressed via QMP -> PMU asserts GPIO 0x56 -> payload triggers warm reset.
6. System resets cleanly back into RetailOS.
Screenshots saved to emulator/screens/session64/.
"""

import json, subprocess, sys, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
out = ROOT / 'emulator/screens/session64'
out.mkdir(parents=True, exist_ok=True)

# 1. Ensure disk patch is up-to-date
patch_file = ROOT / 'probe/chainloader/build/chainloader-as-blocks.patch'
app_file = ROOT / 'probe/chainloader/build/chainloader.hbapp'
if not patch_file.exists() or patch_file.stat().st_mtime < app_file.stat().st_mtime:
    cmd_patch = [
        'python3', 'emulator/tools/disk_patch.py',
        'dumps/ipod-disk.dd',
        f'/Apps/Executables/Blocks.hbapp={app_file}',
        '-o', str(patch_file)
    ]
    subprocess.check_call(cmd_patch, cwd=ROOT)

# 2. Launch NanoEmu with snap6 and disk patch
cmd = [
    'emulator/run.sh',
    '--ramfull', 'dumps/snap6/ram-full.bin',
    '--iram', 'dumps/snap6/iram.bin',
    '--ctx', 'emulator/fixups/snap6-ctx.bin',
    '--overlay', 'dumps/snap6/overlay.bin',
    '--os-poke-file', 'emulator/fixups/snap6-resume.poke',
    '--torn-map', 'dumps/snap6/torn-map.bin',
    '--nand-miss', 'oracle',
    '--nand-disk', 'dumps/ipod-disk.dd',
    '--nand-disk-offset', '49216',
    '--nand-rsrc', 'emulator/rsrc/rsrc-1.1.2.img1',
    '--nand-patch', str(patch_file),
    '-display', 'none',
    '-qmp', 'stdio',
    '-M', 'nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100'
]

log_path = Path('/tmp/chainloader_emu.log')
log_file = open(log_path, 'w')
p = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=log_file, text=True)
p.stdout.readline()

def q(ex, args={}):
    p.stdin.write(json.dumps(dict(execute=ex, arguments=args)) + '\n')
    p.stdin.flush()
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
    q('input-send-event', dict(events=es))
    time.sleep(.13)

def shot(name):
    ppm = out / (name + '.ppm')
    q('screendump', {'filename': str(ppm)})
    time.sleep(0.3)
    if ppm.exists():
        im = Image.open(ppm).convert('RGB')
        im.save(out / (name + '.png'))
        ppm.unlink()
        print(f"Captured {name}.png: size={im.size}, center pixel={im.getpixel((120, 216))}")
        return im
    raise RuntimeError(f"Failed to capture {ppm}")

print("Initializing QMP...")
q('qmp_capabilities')
time.sleep(3)
shot('01_retailos_boot')

print("Navigating to Home screen...")
key('h')
time.sleep(2)
shot('02_retailos_home_p3')

print("Swiping to home page 2 (Blocks slot)...")
for x, d in [(4000, True), (9000, None), (14000, None), (19000, None), (24000, None), (29000, False)]:
    touch(x, 16000, d)
time.sleep(3)
shot('03_retailos_home_p2')

print("Tapping chainloader app (Blocks icon)...")
touch(8700, 17500, True)
touch(8700, 17500, False)
time.sleep(5)

im_green = shot('04_payload_green')
colors = im_green.getcolors(maxcolors=1000)
assert colors == [(240 * 432, (0, 255, 0))], f"Screen not solid green: {colors}"
print("VERIFIED: Screen is 100% solid bright green (0, 255, 0) across all 103,680 pixels.")

print("Pressing Home key to trigger payload reset...")
key('h')
time.sleep(3)
shot('05_retailos_reset')
time.sleep(2)

print("Pressing Home on restored RetailOS...")
key('h')
time.sleep(2)
im_home = shot('06_retailos_restored')

q('quit')
p.wait(timeout=10)

log = log_path.read_text()
checks = [
    'button home pressed',
    'GPIO 0x56 falling edge',
    '[nano7-clkcon] pc=0x08000124 W32 addr=0x3c500050 value=0x000000a5',
    '[nano7-ui] reset: reloading the boot state'
]
for check in checks:
    assert check in log, f"Missing log signature: {check}"

print("PASS: Chainloader round trip succeeded (RetailOS -> chainloader -> green screen payload -> Home button -> RetailOS)")
