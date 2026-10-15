# ViceNano M2-M3 Renderer: Performance Tricks Extracted from Mario Kart 64 Port (30fps on iPod nano 7G)
1. GEOMETRY SUBMISSION (Client Arrays + Deferred Batching):
   - Frontend walks display lists on CPU; backend (`gfx_gles.c:151-221`) converts vertices to homogeneous `GlVtx` (x*w, y*w, z*w, w, uv, rgba).
   - Client vertex arrays (`glVertexPointer`, `glColorPointer`, `glTexCoordPointer`) + `glDrawArrays(GL_TRIANGLES)` (`gfx_gles.c:387, 428`). NO VBOs/DLs; avoids `glDrawElements` which blocks synchronously on OS semaphores (`gl.c:363-376`).
   - Batching by state: consecutive tris with same `DrawState` coalesce into `sStream` (up to 768 verts). State shadowed in `sGL` (`gfx_gles.c:239-300`) to skip redundant GL traps (~0.1ms driver cost each).
   - Texture-before-draw rule (`gfx_gles.c:432-435`): opaque geometry is buffered in `sOpV` (6144 verts) and deferred to frame-end (`op_draw_all`). Mixing uploads between draws reboots the iPod GL driver!
2. TEXTURE HANDLING (16bpp Native, Strict Limits, Respec in Place):
   - Formats: strictly 16bpp in VRAM (`gfx_gles.c:785`): RGBA5551 (`GL_UNSIGNED_SHORT_5_5_5_1`) and RGBA4444 (`GL_UNSIGNED_SHORT_4_4_4_4`). CI8 paletted decoded via CPU TLUT before upload (`gfx_retro_dc.c:1321`). 0 bytes 32bpp.
   - Sizing & Aspect: padded to POT (min 8x8); max dim 256 (`MAX_TEX_DIM = 256`, line 678); max aspect 4:1 (`sMaxAspect = 4`, line 690; e.g. 256x8 crashes, must pad to 256x64). Max upload scratch 64KB (`PAD_TEXELS = 256*128`).
   - Hard Live Texture Ceiling: `LIVE_TEX_HARD = 72` (soft 40; lines 110-114). Driver panics/reboots at >80 textures. Budget = 2048 KB (`TEX_BUDGET`).
   - Slot Versioning & Respec: 8 versions/slot (`TEX_VERS = 8`, lines 95-106) with FNV-1a 64-bit content hash (`h1, h2`). Existing poses swap `name` with 0 GL calls; new frames re-spec in place via `glTexImage2D` to avoid `glGen`/`glDelete` churn.
   - Virtual ROM (`vrom.c:1-40`): 7.2MB textures stored in <=64KB files on NAND; paged on-demand into ~1MB LRU RAM pool (128x 8KB small + 4x 64KB big slots).
3. CUTS & SIMPLIFICATIONS (Software T&L, CPU Culling, Alpha Merging):
   - Fixed-function GLES 1.1 (`glDisable(GL_LIGHTING)` line 998): vertex lighting, N64 combiner, and fog (`r=(r*nf+sFogR*f)/255`, lines 213-217) pre-computed on CPU. Specular offset dropped; sound cut (`port_audio_off=1`).
   - CPU Culling: `glDisable(GL_CULL_FACE)` (line 997); 2D cross-product backface cull with fast `approx_recip_sign(w)` (`gfx_retro_dc.c:2593`), 6-plane clip rejection (`clip_rej`), and software scissor (`gfx_build_clipped_fan`).
   - Degenerate Triangle Cull (`cull_degenerate`, `gfx_gles.c:337-359`): drops triangles with screen area < 0.25 px (~1/8 px) or NaN/w<=0; prevents GPU triangle setup division-by-zero driver panic.
   - No HW Alpha Test: PT cutouts merged with translucent (TR) pass, sorted far-to-near (`qsort`, line 556) with `glDepthMask(GL_FALSE)` and `GL_SRC_ALPHA` blend (lines 531, 563) so transparent pixels don't occlude backdrop.
4. FRAME STRUCTURE PER 30FPS FRAME (16-20ms Frame Budget on Luigi Raceway):
   - Vertex & Draw Counts: ~2,500-4,500 verts typical (peak 8k; OP 6144 max, TR 3072 max, PT 1024 max); ~60-120 `glDrawArrays` draws (coalesced from ~140 DL state changes; ~0.1ms CPU driver cost per draw).
   - Execution Order (`gles_end_frame`, lines 1066-1071): (1) `tr_flush(pass 0)` far backdrop (sky z < 1e-4); (2) `op_draw_all()` batched opaque; (3) `tr_flush(pass 1)` sorted PT cutouts + TR translucent.
   - 30fps Pacing (`port_nano_frame`, `nano_platform.c:377`): when frame completes within 16.6ms, `port_game_replay()` redraws cached DL on in-between 60Hz VBL ticks without ticking game logic.
5. VICENANO M2-M3 RENDERER CHECKLIST:
   [1] Batch into client arrays (`glDrawArrays`), cap draws <=100/frame. [2] Shadow GL state (`sGL`) to eliminate redundant calls. [3] Defer all draws until frame end; never upload textures between draws. [4] 16bpp only (565/4444), clamp POT, max 256x256, max aspect 4:1. [5] Cap active textures <=72, reuse/respec via `glTexImage2D`. [6] CPU backface + degenerate area cull (<0.25 px). [7] Sorted PT+TR alpha pass with `glDepthMask(0)`. [8] NAND streaming via <=64KB chunks in ~1MB LRU pool. [9] Call `hb_fpu_attach_current_task()` (`mk64_entry.c:172`) every frame before float code.
