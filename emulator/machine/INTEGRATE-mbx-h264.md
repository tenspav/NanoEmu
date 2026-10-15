# MBX / Apple M2 draft integration

Only the four device source/header files and this note were created. The
machine/build snippets and vendor tree have not been edited.

## Source limits

`docs/research/gpu-mbx-spec.md` is absent. MBX behavior follows `gpu-mbx.md`;
the MMU status handshake and FIFO barrier event mask are draft assumptions
that need hardware trace confirmation. No unavailable checklist was verified.

`h264-deep.md` corrects the older decoder reference: **0x396xxxxx / IRQ45
is MPEG-4/H.263; actual H.264 is 0x38f00000 / IRQ35**. The requested
`nano7_h264` filenames/type/API are retained, but this model implements only
the requested IRQ45 block. It cannot initialize or execute the real IRQ35
H.264 driver. Do not alias it onto 0x38f00000.

## Lines to add (by the integrating agent)

In `hw/arm/nano7.c`, beside the other device includes (currently near lines
43-44), add:

```c
#include "nano7_mbx.h"
#include "nano7_h264.h"
```

In machine initialization, after VIC creation and the display attach
(currently near lines 536-538), add:

```c
nano7_mbx_attach(get_system_memory(), s->vic_in[NANO7_IRQ_MBX]);
nano7_h264_attach(get_system_memory(), 0x39600000,
                  s->vic_in[NANO7_IRQ_H264]);
```

These helpers realize SysBus devices and map into the supplied parent.
MBX maps 4 KB each at 0x39100000/0x39200000/0x39300000, 16 KB at
0x3b000000, and 2 MB each at 0x3b800000/0x3ba00000/0x3bc00000.
M2 maps [0x39600000, 0x39670000), including reserved sub-block gaps.
If concurrent machine edits add generic mappings at these addresses, remove
those conflicting mappings as part of integration; do not map both models.

In `meson.snippet` line 1, append `'nano7_mbx.c', 'nano7_h264.c'` to the
existing `CONFIG_NANO7` `files(...)` list. No additional libraries are needed.

In `kconfig.snippet`: **no lines to add**; both devices are built under the
existing `config NANO7`. In `default-mak.snippet`: **no lines to add**;
the existing `CONFIG_NANO7=y` enables both.

Copy the new C/header files into the vendor build's `hw/arm` directory only
as part of the later integration workflow; this draft did not do so.

## Behavior and smoke checks

All accesses are aligned little-endian 32-bit MMIO. Unknown registers retain
writes; FIFO reads return zero. Both devices cold-reset and migrate their
register state, restoring the IRQ level after migration. Completion is
synchronous, with no modeled processing delay or completion queue.

MBX initialization: write +0x80=0x111 then 0; check +0xff0=0xb105f00d,
+0xf00=0x01020003, +0xf10=0x00010500. Clear +0x134=0xffff, enable
+0x130=0x8775. MMU table roots +0x1000..+0x101c retain writes;
+0x1020 bit16 mirrors bit0, and invalidation bit8 completes immediately.
Each disabled 2D block reports base bit1; video +0x40 bit0 is ready.

MBX one frame: firmware applies its RAM job's count at +0x20 and
`(offset,value)` pairs at +0x24 to the core, then writes job pointer +0x6ec.
That pointer is retained, not fetched or executed again. +0x808=1 sets
render-done bit4, and IRQ53 follows status & enable. W1C +0x134 clears it.
FIFO occupancy remains zero, giving the documented 0x7b-word capacity.
`0xf0000000` followed by six `0x70000000` words in any single FIFO completes
a barrier with tiled/callback bits 0x420 (assumed event mapping). Enable bit5
explicitly when testing tiled IRQs; initial mask 0x8775 does not enable it.

M2 initialization: +0x1004, +0x30004, +0x30020 read zero; mode, scaling
matrices, slice and profile registers retain writes. Top +0xc=0 resets all
state; +0xc=1 enables readback. Commands are accepted without enforcing gates.

M2 one job: +0x30110=2 or 0x102, or +0x6000c=3, sets child +0x60000 bit1
and top +0 bit6, raising IRQ45. Repeated triggers while pending coalesce.
Child W1C clears child status; top W1C=0x40 lowers IRQ45 independently.
+0x30100 is treated as W1C; separate sequencer arm behavior is a TODO.

There is no RAM descriptor pointer in the doorbell. The firmware setup
record is five u32s `{Y/input, chroma, plane_separation, aux0, aux1}`;
firmware expands it into output/work registers. The input end at +0x6001c
is an address, not a size. DMA/output pointers mask bit31. The seven-u32
H.264 surface record belongs to the other engine and is documented in the
source only, not interpreted here.

TODOs: MBX packet decoding, MMU page walks/faults, rasterization, scaling;
M2 bitstream parsing, decoding, output layout and guest-plane writes.
No guest pixels/bitstream data are touched; these checks demonstrate control
flow only and cannot establish successful real firmware init or playback.
