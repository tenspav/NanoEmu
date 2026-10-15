 equivalent: `rasterCreate/Lock/Unlock/NumLevels`, `imageFindRasterFormat`, `rasterFromImage/rasterToImage`; native ctor/dtor/copy and texture read/write/size functions. Driver palette-lock slots need deliberate handling; GL3 does not install them. [Raster implementation](https://raw.githubusercontent.com/aap/librw/master/src/gl/gl3raster.cpp).
- `gl3immed.cpp` equivalent: `im2DRenderLine/Triangle/Primitive/IndexedPrimitive`; `im3DTransform/RenderPrimitive/RenderIndexedPrimitive/End`; immediate-buffer open/close. Preserve screen Z and reciprocal-camera-Z semantics. [Declarations](https://raw.githubusercontent.com/aap/librw/master/src/gl/rwgl3impl.h).
- `gl3pipe.cpp` equivalent: `ObjPipeline::init/create`, impl `instance/uninstance/render`; `defaultInstanceCB/defaultUninstanceCB/defaultRenderCB`, `makeDefaultPipeline`, native-data destruction; reinstance dirty geometry and draw material batches. [Pipeline](https://raw.githubusercontent.com/aap/librw/master/src/gl/gl3pipe.cpp).
- `gl3render.cpp` equivalent: fixed-function matrix, light, material, texture and vertex-array setup; cached blend/depth/cull/fog/alpha states; indexed draws. Replace generic attributes with vertex/normal/color/texcoord pointers. [Backend declarations](https://raw.githubusercontent.com/aap/librw/master/src/gl/rwgl3.h).
- `gl3skin.cpp`, `gl3matfx.cpp` equivalents: register skin/MatFX pipelines; CPU skinning and environment-map rendering. `gl3shader.cpp` has no required GLES1 counterpart. [Skin](https://raw.githubusercontent.com/aap/librw/master/src/gl/gl3skin.cpp), [MatFX](https://raw.githubusercontent.com/aap/librw/master/src/gl/gl3matfx.cpp).
- Integration: add platform selection to `rw.h`/`rwbase.h`/`engine.cpp` and CMake; define immediate vertex types; adapt reVC `fakerw`, platform startup and `TexRead.cpp`; remove hardwired Android `GLESv3`/`RW_GL3`.

3. **Capability gaps and replacements**
- **VBOs and multitexture are supported:** GLES1.1 requires buffer objects and at least two texture units; retain VBO/16-bit-index draws and use texture combiners. VAOs/generic attributes are unnecessary. [GLES1.1 specification](https://registry.khronos.org/OpenGL/specs/es/1.1/es_cm_spec_1.1.pdf).
- Shader transforms/lighting/fog/alpha discard → fixed matrices, lighting, fog and `glAlphaFunc`; RW prelight-plus-dynamic-light arithmetic may require CPU-computed vertex colors.
- Bone shaders → CPU-transform positions/normals into a reusable dynamic buffer; preserve bind-pose data and restrict animated population.
- Car reflections → CPU-generated environment UVs plus second texture unit or additive pass; lightmaps → UV2 modulation; complex gloss/rim/Fresnel → approximate or disable.
- Local `extras/custompipes_gl.cpp`, `postfx.cpp`, `screendroplets.cpp` directly use shaders: disable extended pipelines/colour filters/droplets initially; approximate tint/blur with blended quads.
- Render-to-texture/FBOs and DXT/NPOT support are extension-dependent: probe; initially omit reflection cameras/post effects, use POT textures, convert DXT/palettes offline to RGB565/RGBA4444; use PVRTC only if exposed.

4. **Memory: proposed 40 MiB ceiling, not measured feasibility**
- Allocate **12 textures + 6 geometry + 10 code/game pools/collision/scripts + 3 audio + 3 streaming/decode scratch + 1 display/depth + 5 reserve = 40 MiB**; count GPU allocations against shared RAM.
- Texture cost with full mipmaps: `W×H×bytes×4/3`; 256² RGB565 ≈171 KiB, 128² ≈43 KiB; 12 MiB holds ~72 or ~288 respectively, before metadata/driver overhead.
- At 36-byte vertices and roughly six 16-bit indices/vertex, 6 MiB holds ~131k resident vertices with one packed copy; RW source arrays plus CPU staging plus VBO can cut that substantially. Reuse/free staging and measure allocations.
- [Streaming.cpp:225](/home/pavliukov/ipodwork/NanoRework/games/reVC/src/core/Streaming.cpp:225) imposes ≥65 MiB streaming alone; accounting uses IMG sector sizes, not actual expanded allocations. Replace the floor, instrument live bytes, bound scratch/archive entries, reduce draw distance/traffic and stream aggressively.
- Mobile asset shrinkage **cannot be established from this tree**: mobile scripts/filter modes exist, but no shipped-mobile asset comparison or reduction pipeline. Downsampling, 16-bit/PVRTC conversion, stronger LODs and smaller resident sets are proposed port work.

5. **Milestones to first spinning 3D render**
- M0: validate the supplied hardware assumptions through the actual device API; obtain GLES1 context, clear/present, log limits/extensions and memory.
- M1: standalone rotating indexed cube with depth/culling and fixed matrices; then add one RGB565 texture.
- M2: wire librw Device/Driver lifecycle and Im3D; render the same cube through librw, checking coordinate conventions and state restoration.
- M3: default atomic pipeline plus raster upload; load one small non-native DFF/TXD and rotate it-**first asset render**, before game startup, skinning or world streaming.
