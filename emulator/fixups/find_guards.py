# find_guards.py -- list __cxa_guard_acquire (0x842d3c8) guard words in the
# OS image and classify those guarding BSS objects. Expects /tmp/os.bin =
# first 12MB of the RAM dump. Usage: python3 -I find_guards.py [acquire_addr]
import re, struct, subprocess, sys
img = open('/tmp/os.bin','rb').read()
BASE=0x08000000
def w(a): return struct.unpack_from('<I', img, a-BASE)[0]
END=0x08800000
dis = subprocess.run(['arm-none-eabi-objdump','-D','-b','binary','-marm','-Mforce-thumb',
      '--adjust-vma=0x08000000','--start-address=0x08000000','--stop-address=%#x'%END,'/tmp/os.bin'],
      capture_output=True,text=True).stdout.splitlines()
ins=[]
for l in dis:
    m=re.match(r'\s*([0-9a-f]+):\s+(?:[0-9a-f]{4} ?){1,2}\s+(\S+)\s*(.*)',l)
    if m: ins.append((int(m.group(1),16),m.group(2),m.group(3)))
idx={a:i for i,(a,_,_) in enumerate(ins)}
TARGET=int(sys.argv[1],16) if len(sys.argv)>1 else 0x842d3c8
def resolve(i, reg, depth=0):
    for j in range(i-1, max(i-14,-1), -1):
        a,op,args=ins[j]
        if op in ('bl','blx','b.n','b.w','bx') : return None
        parts=[p.strip() for p in args.split(';')[0].split(',')]
        if not parts or parts[0]!=reg: continue
        if op.startswith('ldr') and '[pc' in args:
            m=re.search(r'\(0x([0-9a-f]+)\)|; 0x([0-9a-f]+)',args)
            if m:
                la=int(m.group(1) or m.group(2),16); return w(la)
            return None
        if op in ('adds','add','add.w','addw','subs','sub','sub.w','subw'):
            if len(parts)==2: src,imm=reg,parts[1]
            else: src,imm=parts[1],parts[2]
            if not imm.startswith('#'): return None
            v=resolve(j,src,depth+1)
            if v is None: return None
            n=int(imm[1:],0); return v+n if op.startswith('add') else v-n
        if op in ('mov','movs','mov.w') and parts[1].startswith('r'):
            return resolve(j,parts[1],depth+1)
        return None
    return None
res=[]
for i,(a,op,args) in enumerate(ins):
    if op in ('bl','blx') and args.startswith('0x') and int(args.split()[0],16)==TARGET:
        g=resolve(i,'r0')
        res.append((a,g))
unres=[a for a,g in res if g is None]
print('calls',len(res),'unresolved',len(unres), [hex(x) for x in unres[:20]])
gs=sorted(set(g for a,g in res if g is not None))
for g in gs:
    if BASE<=g<BASE+len(img):
        print(hex(g), w(g))

# --- classify: guard -> object literals near the acquire call
BSS0,BSS1=0x08971704,0x08999020
lit_re=re.compile(r'\(0x([0-9a-f]+)\)')
cls={}
for a,g in res:
    i=idx[a]; objs=set()
    for j in range(max(0,i-24), min(len(ins),i+24)):
        aa,op,args=ins[j]
        if op.startswith('ldr') and '[pc' in args:
            m=lit_re.search(args)
            if m:
                v=w(int(m.group(1),16))
                if v!=g and v!=g-4 and BASE<=v<0x0a000000: objs.add(v)
    c=cls.setdefault(g,set()); c|=objs
bssg=[g for g in gs if w(g)==1 and any(BSS0<=o<BSS1 for o in cls.get(g,()))]
nob=[g for g in gs if w(g)==1 and g not in bssg]
print('BSSGUARDS', len(bssg), ':'.join(hex(g) for g in bssg))
print('OTHER', len(nob), ' '.join(hex(g) for g in nob[:40]))
