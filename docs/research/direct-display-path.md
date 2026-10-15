================================================================================
IPOD NANO 7G (S5L8747): DIRECT-TO-PANEL DISPLAY INVESTIGATION (snap6 / RetailOS)
================================================================================

1. CAN AN APP WRITE LCD WINDOW REGISTERS DIRECTLY (0x38900024 / 0x38900044)?
--------------------------------------------------------------------------------
- Yes, MMIO registers are fully writable, BUT YOUR BUFFER WILL NOT PERSIST.
- Why: Panel Tearing Effect (TE) pulses at ~60 Hz (16.67 ms). GPIO Pin 14 (0x0e)
  fires VIC IRQ 32 -> ISR 0x083a33be -> callback table slot [0x08ae6244]
  (points to 0x08060b19 -> 0x0807558c).
- Every frame, 0x0807558c unconditionally invokes commit function 0x08080b70:
    * Window 0: reads active surface from display_object->ring[cur_idx], calls
      surface->getBuffer() & 0x7fffffff (returns OS buffer 0x0bf29380/0x0bec3f00),
      writes it to 0x38900024, and sets 0x38900004 |= 0x10 (Window 0 enable).
      -> Any value you wrote to 0x38900024 is overwritten on the next TE pulse.
    * Window 1: checks display_object->win1_idx (offset +0x268). In stock OS,
      no second layer is active (+0x268 == -1). The code branches to 0x08080d1a:
      bic r0, r0, #0x20; str r0, [0x38900004], EXPLICITLY DISABLING WINDOW 1.
- Conclusion: Under unmodified OS operation, the OS immediately reclaims
  0x38900024 and disables 0x38900044 on the very next frame (~16.6 ms).

2. VBLANK / TE SYNC AN APP CAN WAIT ON
--------------------------------------------------------------------------------
- Panel TE line is wired to GPIO 0x0e (Bank 1, Pin 6).
- Direct Level Polling:
    * Register: 0x3cf00024 (PDAT1), Bit 6 (mask 0x40).
    * Polarity: HIGH (1) during active pixel scan-out; LOW (0) during ~1 ms
      vertical blanking interval (TE pulse).
    * Pacing loop:
        while ((*(volatile uint32_t *)0x3cf00024 & 0x40) != 0); // wait for vblank
- Latched Interrupt Status (W1C):
    * Register: 0x39700084 (GPIO Group 1 Interrupt Pending), Bit 6 (mask 0x40).
    * Latches on falling edge; acknowledge/clear by writing 0x40 back:
        while ((*(volatile uint32_t *)0x39700084 & 0x40) == 0);
        *(volatile uint32_t *)0x39700084 = 0x40; // clear pending

3. WHAT BREAKS IF THE OS REDRAWS & HOW TO TAKE/KEEP CLEAN OWNERSHIP
--------------------------------------------------------------------------------
- What breaks: Thread 64 (GKCompositor_CLCD_G) renders dirty UI into 0x0bf29380
  and sets display_object+0x770 = 1. At the next TE pulse, 0x0807558c overwrites
  LCD window registers via 0x08080b70, then calls 0x080a06c8 which triggers bridge
  0x38300080 = 1 and sends DCS 0x2a/0x2b/0x2c via DSI 0x3d800000, flushing the OS
  framebuffer across DSI into panel GRAM. This causes immediate tearing/flicker.
- How to keep ownership cleanly:
    * Cleanest Hardware-Level Suppression:
      Display object pointer is stored in RAM at [0x08ae642c] (0x0998a9c4 in snap6).
      Offset +0x772 is the display quiesce/sleep flag.
      At 0x080755b4, the TE callback checks: ldrb.w r0, [r4, #0x772]; cbz ...
      When +0x772 is set to 1, 0x0807558c skips BOTH the register commit (0x08080b70)
      and the DSI panel push (0x080a06c8) and returns immediately!
    * Alternative RAM Callback Unhook:
      Save orig = *(uint32_t *)0x08ae6244; set *(uint32_t *)0x08ae6244 = 0;
      (silences OS TE callback entirely; restore on app exit).

4. MINIMAL 60FPS DIRECTDRAW CODE SEQUENCE (<25 LINES, CONCRETE REGISTERS)
--------------------------------------------------------------------------------
// iPod nano 7G DirectDraw: 60fps direct-to-panel, zero OS interference
void directdraw_60fps(uint32_t *fb0, uint32_t *fb1) {
    volatile uint32_t *obj = *(volatile uint32_t **)0x08ae642c; // display object
    *(volatile uint8_t *)((uint32_t)obj + 0x772) = 1;          // suppress OS TE commit/push
    volatile uint32_t *lcd = (volatile uint32_t *)0x38900000;
    lcd[0x04/4] |= 0x10;                                       // enable Window 0
    void (*dsi_push)(int) = (void (*)(int))0x080a06c8;         // firmware bridge+DSI push
    uint32_t *cur = fb0;
    while (!should_quit()) {
        while ((*(volatile uint32_t *)0x3cf00024 & 0x40) != 0);// wait TE falling edge (vblank)
        lcd[0x24/4] = (uint32_t)cur & 0x7fffffff;             // flip Window 0 base address
        dsi_push(1);                                           // bridge 0x38300080 DMA push to GRAM
        render_game_frame(cur);                                // draw 240x432 BGRA8888 pixels
        cur = (cur == fb0) ? fb1 : fb0;                        // swap double buffer
    }
    *(volatile uint8_t *)((uint32_t)obj + 0x772) = 0;          // restore OS compositor
}
