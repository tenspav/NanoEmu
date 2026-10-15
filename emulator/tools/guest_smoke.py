#!/usr/bin/env python3
"""guest_smoke.py -- drive a running-from-snapshot NanoEmu over QMP and
check that guest apps open (Music, Settings, a homebrew game).

  guest_smoke.py OUTDIR STEPS -- EMULATOR COMMAND... (must include -qmp stdio)

STEPS: comma-separated: wait:SEC, key:QCODE, tap:X:Y (panel pixels, no
--frame), atap:FX:FY (fractions 0..1 of the whole console, e.g. a button
drawn by --frame),
swipe:X0:X1:Y, hmp:MONITOR-COMMAND, shot:NAME (PPM of the console into OUTDIR).
Stderr of the emulator goes to OUTDIR/emu.log; at the end the log is
scanned for resets / aborts. No third-party modules (runs on stock macOS).
"""
import json, os, subprocess, sys, time

out, steps = sys.argv[1], sys.argv[2]
cmd = sys.argv[sys.argv.index('--') + 1:]
os.makedirs(out, exist_ok=True)
log = open(os.path.join(out, 'emu.log'), 'w')
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=log, text=True)
p.stdout.readline()

def q(ex, args=None):
    p.stdin.write(json.dumps({'execute': ex, 'arguments': args or {}}) + '\n')
    p.stdin.flush()
    while True:
        r = json.loads(p.stdout.readline())
        if 'return' in r or 'error' in r:
            return r

def touch(x, y, down=None, frac=False):
    if not frac:
        x, y = x / 239, y / 431
    ev = [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767)}},
          {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767)}}]
    if down is not None:
        ev.append({'type': 'btn', 'data': {'down': down, 'button': 'left'}})
    q('input-send-event', {'events': ev})
    time.sleep(.13)

q('qmp_capabilities')
t0 = time.time()
for st in steps.split(','):
    a = st.split(':')
    if a[0] == 'wait':
        time.sleep(float(a[1]))
    elif a[0] == 'key':
        q('send-key', {'keys': [{'type': 'qcode', 'data': a[1]}]})
        time.sleep(.3)
    elif a[0] == 'tap':
        touch(int(a[1]), int(a[2]), True)
        touch(int(a[1]), int(a[2]), False)
    elif a[0] == 'atap':
        touch(float(a[1]), float(a[2]), True, True)
        touch(float(a[1]), float(a[2]), False, True)
    elif a[0] == 'swipe':
        x0, x1, y = int(a[1]), int(a[2]), int(a[3])
        for i in range(7):
            touch(x0 + (x1 - x0) * i // 6, y,
                  True if i == 0 else (False if i == 6 else None))
    elif a[0] == 'hmp':         # hmp:info registers (output printed)
        r = q('human-monitor-command', {'command-line': ':'.join(a[1:])})
        print(r.get('return', r), flush=True)
    elif a[0] == 'shot':
        q('screendump', {'filename': os.path.abspath(os.path.join(out, a[1] + '.ppm'))})
    print('%6.1f %s' % (time.time() - t0, st), flush=True)
q('quit')
p.wait(timeout=20)
log.close()
text = open(os.path.join(out, 'emu.log'), errors='replace').read()
for sig in ('reset: reloading', 'abort', 'panic', 'Data Abort', 'undefined'):
    n = text.count(sig)
    if n:
        print('LOG: %d x %r' % (n, sig))
for line in text.splitlines():
    if 'summary' in line and ('fmc' in line or 'i2s' in line or 'adsp' in line) or 'NAND store' in line:
        print('LOG:', line[:200])
