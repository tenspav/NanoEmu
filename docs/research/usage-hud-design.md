# NanoEmu In-Emulator Usage HUD Design & Implementation Plan
1. HUD PLACEMENT & TOGGLE: Docked right-side panel (80x432 px alongside 240x432 LCD, console expands to 320x432) or semi-transparent top-right 80x112 px overlay; toggled via F8 (unmapped between F7 rotate and F9 zoom; avoids guest touch collision in nano7_ui_map_touch).
2. DATA SOURCES & SAMPLING:
   - CPU Busy %: Ratio of active TCG execution vs WFI idle loop (cpu->halted / idle PC 0x08003xxx) from machine PC sampler over sample window: busy% = (ticks_active / ticks_total) * 100.
   - Guest Heap Usage: Read main-heap manager pointer at physical 0x08909134 -> object 0x089a6228, managed span at 0x089a66bc (40.299 MiB). Traverse ~167 free blocks (bit 0 = 1 free flag in block header) directly via memory_region_get_ram_ptr(dram) to sum free bytes; heap% = (managed - free) / managed * 100.
   - GPU Busy %: Accumulate job execution time in nano7_mbxr.c (doorbell start to job completion ns); gpu% = (job_duration_ns / window_duration_ns) * 100.
3. UPDATE RATE & PERFORMANCE BUDGET:
   - Sampling rate: 4 Hz (250 ms QEMUTimer); sparkline history ring buffer of 60 points (15 s window).
   - Cost: Zero emulation slowdown. PC sampler and MBX job timers update lockless counters on event. Heap walk scans only ~167 headers in host RAM (<5 µs). Rendering is ~900 pixels blit (<15 µs). Total host CPU overhead <0.02%.
4. VISUAL STYLE & GRAPH LAYOUT:
   - Palette: Monochrome dark teal theme. Background 0xDD081418 (dark slate teal), frame border 0xFF164E63 (dim teal), active bars/graphs 0xFF14B8A6 (teal), sparkline trace 0xFF2DD4BF / peak 0xFF5EEAD4.
   - Layout: 3 horizontal bar graphs (CPU, RAM, GPU: 48x6 px bar with 1px outline + fill + text % using 4x6 bitmap font) + 60x24 px scrolling history sparkline (dual-line CPU & GPU load) at bottom.
5. IMPLEMENTATION PLAN IN nano7_ui.c:
   - (a) State: Add Nano7HudState (enabled bool, 250ms QEMUTimer, rolling 60-sample ring buffers for CPU/RAM/GPU, last snapshot values) to Nano7UiState.
   - (b) Input: Bind Q_KEY_CODE_F8 in ui_key_event() to toggle HUD visibility; if side panel, resize console surface to (w + 80*zoom, h) in ui_render(); if overlay, composite directly into ui->out.
   - (c) Sampler Hook: On timer callback, poll PC sampler active ratio, query nano7_mbxr_get_busy(), and walk DRAM heap at 0x08909134.
   - (d) Compositor: Add 4x6 ASCII font table and micro-primitives (hud_rect, hud_bar, hud_sparkline); in ui_render(), blit HUD surface onto ui->out before dpy_gfx_update_full().
