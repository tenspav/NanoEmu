# NanoGL, the PowerVR MBX Lite renderer

NanoGL is NanoEmu's reimplementation of the iPod nano 7G's GPU pipeline (PowerVR MBX Lite, OpenGL ES 1.1 class, fixed function). It lives in `emulator/machine/hw/arm/nano7_mbx.c` (register/FIFO front-end) and `nano7_mbxr.c` (the renderer).

<p align="center"><img src="../assets/nanogl-logo-256.png" width="192" alt="NanoGL"></p>

## Supported today (validated against gltest variants + real 3D homebrew)

| Feature | Status | Proof |
|---|---|---|
| Geometry, projection, viewport | ✅ exact | analytic reference IoU 1.000 |
| Textures (upload + sampling) | ✅ | Blocks world, textured quads |
| Depth test | ✅ | Blocks renders correctly |
| Alpha blending | ✅ exact alpha values | Blocks HUD semi-transparent |
| Alpha test | ✅ | gltest variants 22-24 |
| Back-face culling | ✅ | gltest v12/v20 |
| Bilinear filtering (single filter bit) | ✅ | gltest v27 smooth edges |
| Mipmaps (uploaded levels) | ✅ | S37 |
| Near-plane clipping | ✅ | S38: floor crossing camera plane |
| glColorMask | ✅ | S41 |
| Fixed-function lighting | ✅ | S43 |
| GL_BYTE / GL_SHORT normals | ✅ | S44 |
| GL_POINTS / point sprites | ✅ | S46 |
| Fog (LINEAR / EXP / EXP2) | ✅ exact factors | S42 |
| PVRTC 4bpp / 2bpp (RGB + RGBA) | ✅ bit-exact vs an independent decoder | S63: gltest 78-81 |
| Paletted 4bpp / 8bpp (all 5 palette formats) | ✅ exact (the driver expands them on the CPU) | S63: gltest 82-87 |

## Verified as "driver never sends it" (nothing to model)

- Two-sided lighting (S45)
- Polygon offset (S47)
- Stencil (the OS GL driver has no stencil path, S39)

## Known gaps / in progress

- Display-stage darkening of the GL layer (cosmetic, under investigation)
- Texture filtering mip-filter combos beyond the single filter bit
- PVRTC mip chains: decoded assuming levels follow each other (the driver uploads one level per call; unverified)
- Full-GL games validation (MK64) as the ultimate stress test

## Real-world validation

- **gltest** (custom app, 87 variants probing one feature each)
- **Blocks** (homebrew 3D game): world + textures + translucent HUD all correct
- Next: Mario Kart 64 as the final exam
