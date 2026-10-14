1. nano7_h264.h:8-11: Wrong hardware block/IRQ. 0x396xxxxx/IRQ45 is MPEG-4/H.263; real H.264 is at 0x38f00000/IRQ35 per h264-deep.md. Real H.264 driver cannot run on this model.
   Fix: Retarget as nano7_m4v and implement dedicated 0x38f00000/IRQ35 device for H.264.
2. nano7_mbx.c:149: MMU semantic bug. +0x1020 write zeroes bits 0 and 16 when bit 8 (invalidation) is written without bit 0, disabling MMU translation and failing driver poll at 0x08484e94.
   Fix: Preserve translation enable and status bits (0 and 16) across invalidation writes.
3. nano7_h264.c:102: Wrong IRQ usage. Doorbell 0x39630110 write invokes m2_complete(), asserting frame-done (+0x00 bit 6, +0x60000 bit 1, IRQ45) before step 5 decode trigger 0x3966000c=3.
   Fix: Do not call m2_complete() on doorbell 0x30110; trigger completion only on 0x6000c=3.
4. nano7_h264.c:38: Wrong IRQ usage. m2_update_irq ignores +0x04 (Top IRQ enable mask 0x142), raising IRQ45 even when masked by firmware.
   Fix: Gate top status bits with s->regs[0x04/4] before evaluating the IRQ line level.
5. nano7_mbx.c:95: MMIO semantic bug. Writing window 2 +0x40 (0x39300040) overwrites read-only Video Output status, clearing ready bit 0 set at reset and causing 0x08253004 poll to hang.
   Fix: Ignore writes to w->index==2 && off==0x40 or preserve bit 0.
6. nano7_mbx.c:203 & nano7_h264.c:164: Lifecycle/SysBus bug. Reset called in instance_init before sysbus_connect_irq connects s->irq (fires IRQ on NULL); dc->reset is deprecated in QEMU 9.1.2.
   Fix: Remove reset from instance_init; implement ResettableClass or repo-wide non-SysBus pattern.
7. nano7_mbx.c:215 & nano7_h264.c:176: Migration/API mismatch. SysBus dc->vmsd uses dynamic anonymous paths (/machine/peripheral-anon/...) breaking nano7_ui save-state; H264 saves 458KB sparse array.
   Fix: Use vmstate_register_any(NULL) like nano7_disp/fmc and compact H264 state.
8. nano7_mbx.c:163-165 & nano7_h264.c:135-137: Endianness/MMIO size bug. Uses DEVICE_LITTLE_ENDIAN (all 16 other devices use DEVICE_NATIVE_ENDIAN) and valid.min_access_size=4 rejects byte MMIO.
   Fix: Switch to DEVICE_NATIVE_ENDIAN, set valid.min_access_size=1 and impl.min_access_size=4.
9. nano7_mbx.c:142 & nano7_h264.c:65: DMA memory read stubbed. Neither models guest memory DMA yet; bounds checks against bitstream end (+0x6001c) and surface size will be required.
   Fix: Add explicit dma_memory_read bounds validation when implementing memory decode/rendering.
