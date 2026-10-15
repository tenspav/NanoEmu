from PIL import Image, ImageDraw, ImageFilter
import math
S=2048; BG=(10,18,32); T=(20,184,166); C=(103,232,249)
def layer(): return Image.new("RGBA",(S,S),(0,0,0,0))
base=Image.new("RGBA",(S,S),BG+(255,))
# subtle radial vignette
v=layer(); d=ImageDraw.Draw(v)
d.ellipse((300,300,S-300,S-300),fill=(20,184,166,40)); v=v.filter(ImageFilter.GaussianBlur(250)); base.alpha_composite(v)
glow=layer(); g=ImageDraw.Draw(glow); crisp=layer(); c=ImageDraw.Draw(crisp)
def both(fn):
    fn(g,1); fn(c,0)
# ipod body
x0,y0,x1,y1=600,260,1448,1788
def body(d,gl):
    w=44
    d.rounded_rectangle((x0,y0,x1,y1),radius=140,outline=T+(255,),width=w)
both(body)
# screen
sx0,sy0,sx1,sy1=690,360,1358,1180
def scr(d,gl): d.rounded_rectangle((sx0,sy0,sx1,sy1),radius=50,outline=T+(255,),width=18)
both(scr)
# cube: true 3D rotation, perspective-free
cx,cy=1024,770; k=190
import itertools
V=[(x,y,z) for x in(-1,1) for y in(-1,1) for z in(-1,1)]
ax,ay=math.radians(24),math.radians(35)
def proj(v):
    x,y,z=v
    x,z=x*math.cos(ay)+z*math.sin(ay),-x*math.sin(ay)+z*math.cos(ay)
    y,z=y*math.cos(ax)-z*math.sin(ax),y*math.sin(ax)+z*math.cos(ax)
    return (cx+k*x,cy+k*y)
PV=[proj(v) for v in V]
E_=[(i,j) for i in range(8) for j in range(i+1,8) if sum(a!=b for a,b in zip(V[i],V[j]))==1]
def cube(d,gl):
    w=26 if gl else 14
    for i,j in E_: d.line([PV[i],PV[j]],fill=C+(255,),width=w)
    # low-poly triangulation on one face (z=+1 side)
    f=[i for i,v in enumerate(V) if v[2]==1]
    d.line([PV[f[0]],PV[f[3]]],fill=C+(190,),width=w-6)
    for p in PV: d.ellipse((p[0]-22,p[1]-22,p[0]+22,p[1]+22),fill=(255,255,255,255))
both(cube)
# home button
def wheel(d,gl):
    d.ellipse((1024-110,1500-110,1024+110,1500+110),outline=T+(255,),width=24)
both(wheel)
glow=glow.filter(ImageFilter.GaussianBlur(28))
base.alpha_composite(glow); base.alpha_composite(crisp)
o=base.resize((1024,1024),Image.LANCZOS).convert("RGB")
o.save("/home/pavliukov/ipodwork/NanoRework/docs/nanogl-logo.png")
o.resize((256,256),Image.LANCZOS).save("/home/pavliukov/ipodwork/NanoRework/docs/nanogl-logo-256.png")
