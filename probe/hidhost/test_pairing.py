#!/usr/bin/env python3
"""NanoEmu pairing regression. Run from repository root after make and disk_patch.
Uses QMP stdio (no local sockets). Screens and HCI evidence go to screens/session51.
"""
import subprocess,json,time
from pathlib import Path
from PIL import Image
out = Path('probe/hidhost/screens/session51')
out.mkdir(parents=True, exist_ok=True)
cmd='emulator/run.sh --ramfull dumps/snap6/ram-full.bin --iram dumps/snap6/iram.bin --ctx emulator/fixups/snap6-ctx.bin --overlay dumps/snap6/overlay.bin --os-poke-file emulator/fixups/snap6-resume.poke --torn-map dumps/snap6/torn-map.bin --nand-miss oracle --nand-disk dumps/ipod-disk.dd --nand-disk-offset 49216 --nand-rsrc emulator/rsrc/rsrc-1.1.2.img1 --nand-patch probe/hidhost/build/hidhost-as-blocks.patch --bt-peer gamepad --bt-pad 2:lx=127:1200/5:a+ly=-100:1500/9:b:1000 -display none -qmp stdio -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100'
p=subprocess.Popen(cmd.split(),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=open('/tmp/hh51.log','w'),text=True)
p.stdout.readline()
def q(ex,args={}):
 p.stdin.write(json.dumps(dict(execute=ex,arguments=args))+'\n');p.stdin.flush()
 while True:
  r=json.loads(p.stdout.readline())
  if 'return' in r or 'error' in r:return r
q('qmp_capabilities');time.sleep(3);q('human-monitor-command',{'command-line':'sendkey h'});time.sleep(2)
def touch(x,y,down=None):
 es=[dict(type='abs',data=dict(axis='x',value=x)),dict(type='abs',data=dict(axis='y',value=y))]
 if down is not None:es.append(dict(type='btn',data=dict(down=down,button='left')))
 q('input-send-event',dict(events=es));time.sleep(.13)
for x,d in [(4000,True),(9000,None),(14000,None),(19000,None),(24000,None),(29000,False)]:touch(x,16000,d)
time.sleep(3);touch(8700,17500,True);touch(8700,17500,False)
time.sleep(8)
for name, delay in [('right', 3), ('a-up', 4), ('secure-connected', 0)]:
 q('screendump',{'filename':'/tmp/hh51-app.ppm'})
 Image.open('/tmp/hh51-app.ppm').save(out / (name + '.png'))
 time.sleep(delay)
q('quit'); p.wait(timeout=10)
log = Path('/tmp/hh51.log').read_text()
checks = ['Link_Key_Request_Negative_Reply', 'User_Confirmation_Request_Reply',
          'paired with "NanoPad"', 'link encryption on',
          'SDP ServiceSearchAttribute', 'L2CAP PSM 0x11 open',
          'L2CAP PSM 0x13 open', 'HID input report #7']
for check in checks:
 assert check in log, check
lines = [line for line in log.splitlines() if 'nano7-bt' in line]
(out / 'pairing.log').write_text('\n'.join(lines) + '\n')
print('PASS: fresh pairing, encryption, SDP and secure HID reports')
