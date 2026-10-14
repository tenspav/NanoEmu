in.u32', 'q5, <illegal reg q3.5>, q4')
('0x22006ae6', 'vst3cc.32', '{d28[1],d29[1],d30[1]}, [r1], sl')
('0x22006b12', 'vsdot.s8', '<illegal reg q10.5>, q3, q7')
('0x22006bfe', 'vst4.8', '{d3-d6}, [r0], r6')
('0x22007018', 'vshl.u32', 'd1, d31, d8')
('0x2200730e', 'vaba.u8', 'd29, d26, d8')
('0x22007466', 'vmls.i<illegal width 64>', 'q15, <illegal reg q4.5>, <illegal reg q4.5>')
('0x22007514', 'vabal.s8', '<illegal reg q7.5>, d14, d0')
('0x220079ba', 'vqrdmlah.s<illegal width 8>', 'q14, <illegal reg q2.5>, <illegal reg q4.5>')
('0x220079d4', 'vshr.u64', 'd26, d19, #3')
('0x22007b28', 'vext.8', 'q13, <illegal reg q1.5>, q10, #9')
('0x22007c08', 'vst2.32', '')
('0x22007c5a', 'vst1.64', '{d21-d22}, [sp :256], r4')
('0x22007e72', 'vsub.i8', 'd17, d29, d11')
('0x22008016', 'vcmla.f16', 'q14, <illegal reg q5.5>, d9[1], #90')
('0x22008214', 'vrhadd.s32', 'q14, <illegal reg q10.5>, q15')
('0x22008296', 'vqmovn.u16', 'd24, <illegal reg q7.5>')
('0x220082f8', 'vmax.u16', 'q13, <illegal reg q0.5>, <illegal reg q0.5>')
('0x220083cc', 'vmin.u16', 'q10, q14, <illegal reg q8.5>')
('0x22008768', 'vabd.u<illegal width 64>', 'q5, <illegal reg q2.5>, <illegal reg q8.5>')
('0x22008778', 'vshl.u8', 'q13, <illegal reg q7.5>, <illegal reg q7.5>')
('0x220089d2', 'vrshr.u16', 'd9, d21, #2')
('0x22008a10', 'vld4.32', '{d30[],d32[],d34[],d36[]}, [lr :64], r6')
('0x22008aaa', 'vcvt.s32.f32', 'd0, d17, #15')
('0x22008c14', 'vmlsl.u32', 'q2, d1, d3[1]')
('0x22008d22', 'vdup.8', 'q1, sp')
('0x22008fa6', 'vrhadd.s32', '<illegal reg q0.5>, <illegal reg q4.5>, q8')
('0x220092f8', 'vqadd.u32', '<illegal reg q7.5>, q4, q12')
('0x22009336', 'vshl.s8', '<illegal reg q10.5>, <illegal reg q4.5>, <illegal reg q0.5>')
('0x22009372', 'vqrdmlsh.s<illegal width 64>', 'q14, <illegal reg q12.5>, q0')
('0x22009428', 'vmlal.s32', 'q11, d19, d4')
('0x2200956a', 'vshl.s32', 'd8, d20, d12')
('0x22009820', 'vmov.u8', 'r6, d16[7]')
('0x22009930', 'vmul.p<illegal width 64>', 'q15, q0, q12')
('0x220099e4', 'vsub.i32', '<illegal reg q14.5>, q11, <illegal reg q6.5>')
('0x220099f6', 'vmlsl.u16', '<illegal reg q10.5>, d26, d15')
('0x22009ace', 'vext.8', 'q12, <illegal reg q13.5>, q2, #13')
('0x22009d32', 'vld2.8', '{d30[4],d31[4]}, [sl], r7')
('0x2200a15c', 'vqrdmlsh.s<illegal width 8>', '<illegal reg q7.5>, <illegal reg q7.5>, <illegal reg q15.5>')
('0x2200a182', 'vpmax.s32', 'd5, d25, d22')
('0x2200a36a', 'vmul.i<illegal width 64>', '<illegal reg q9.5>, <illegal reg q5.5>, <illegal reg q0.5>')
('0x2200a4cc', 'vcgt.u32', 'q2, <illegal reg q14.5>, <illegal reg q9.5>')
('0x2200a672', 'vmax.f16', 'q14, q3, <illegal reg q12.5>')
('0x2200a8ca', 'vcgt.f32', '<illegal reg q1.5>, q11, <illegal reg q2.5>')
SYS

codex
The OS saves only `d0–d15` during lazy VFP context switches. Its initialization at `0x0862ad78` also enables CP10/11 while setting CPACR’s ASEDIS and D32DIS bits, disabling Advanced SIMD and upper-register access where implemented. I’ve confirmed scalar VFP arithmetic; the remaining scan is checking whether any apparent NEON hits are real code.
exec
/bin/bash -lc "python3 - <<'PY'
import numpy as np,collections
from capstone import *
d=open('dumps/snap6/ram-full.bin','rb').read(); c=Cs(CS_ARCH_ARM,CS_MODE_THUMB)
# find genuine feature reads at either alignment; show context
for name,word in [('fpsid',0xeef00a10),('mvfr0',0xeef70a10),('mvfr1',0xeef60a10),('midr',0xee100f10)]:
 print(name)
 for mode in ['A','T']:
  for al in ([0] if mode=='A' else [0,2]):
   arr=np.frombuffer(d[al:len(d)-((len(d)-al)%4)],dtype='<u4'); vals=arr if mode=='A' else (arr<<16)|(arr>>16)
   idx=np.flatnonzero((vals&0xffff0fff)==word)
   print(mode,[(hex(0x8000000+al+int(j)*4),hex(int(vals[j]))) for j in idx[:25]],'count',len(idx))
PY" in /home/pavliukov/ipodwork/NanoRework
 exited 1 in 0ms:
Traceback (most recent call last):
  File "<stdin>", line 1, in <module>
ModuleNotFoundError: No module named 'numpy'

web search: 
web search: https://documentation-service.arm.com/static/5f04254ddbdee951c1cd8558
  p.wait(); print(file,mode,'v-count',sum(counts.values()),'top',counts.most_common(25),'hot',[(hex(k<<16),v) for k,v in buckets.most_common(20)]); print('SYSTEM',*sys[:200],sep='\\n'); print('EXAMPLES',*examples,sep='\\n')
 print(mode,'total',len(hits),'counts',collections.Counter(x[1] for x in hits).most_common(25),'buckets',collections.Counter(int(x[0],16)>>16 for x in hits).most_common(15),flush=True)
arm total 51182 counts [('vldr', 1618), ('vstr', 1539), ('vmax.s8', 1344), ('vmax.s32', 1034), ('vmax.s<illegal width 64>', 901), ('vst4.8', 825), ('vhadd.s8', 786), ('vadd.i8', 725), ('vext.8', 724), ('vst1.8', 709), ('vmla.f32', 703), ('vmax.s16', 688), ('vadd.i32', 522), ('vqdmulh.s<illegal width 8>', 503), ('vmov.f32', 462), ('vmax.u32', 447), ('vmax.u<illegal width 64>', 437), ('vmov', 431), ('vmax.u16', 416), ('vadd.i16', 413), ('vqadd.s8', 399), ('vhadd.u32', 399), ('vmul.f32', 397), ('vmin.s8', 393), ('vrhadd.s8', 389)] buckets [(2071, 1792), (2074, 1729), (2079, 1520), (2075, 1517), (2077, 1491), (2054, 1288), (2073, 1272), (2076, 1192), (2062, 1190), (2078, 1178), (2052, 1146), (2058, 1066), (2063, 1060), (2061, 1051), (2051, 1040)]
thumb total 39876 counts [('vrhadd.u16', 5744), ('vldr', 5497), ('vstr', 4852), ('vmov', 3599), ('vpop', 1315), ('vpush', 1236), ('vmul.f32', 1116), ('vstmia', 872), ('vldmia', 870), ('vmrs', 811), ('vshr.u64', 791), ('vmla.f32', 776), ('vmov.f32', 709), ('vcvt.f32.s32', 682), ('vsub.f32', 406), ('vmul.f64', 393), ('vadd.f32', 391), ('vcmpe.f32', 365), ('vcvt.f64.f32', 349), ('vdiv.f32', 348), ('vcmpe.f64', 339), ('vmov.f64', 320), ('vcvt.f32.u32', 320), ('vmls.f32', 276), ('vcvt.s32.f32', 246)] buckets [(2087, 1523), (2088, 1176), (2154, 955), (2058, 948), (2118, 878), (2149, 808), (2056, 780), (2074, 764), (2052, 762), (2163, 725), (2073, 704), (2150, 699), (2061, 670), (2147, 647), (2071, 644)]
