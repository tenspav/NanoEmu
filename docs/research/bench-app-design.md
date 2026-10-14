BENCHNANO PROBE SPEC — iPod nano 7G (Samsung S5L8747 Cortex-A5 / MBX Lite)
1. Architecture & UI Flow: Raw-surface app (RAW_SURFACE:=1, HBAppKind 1, 240x432 XRGB8888, 3x5 font). 4 states: IDLE (start/boost buttons) -> RUNNING (sequential test dispatch, progress bar) -> BOOST_WARN (modal warning: thermal/stability risks, tap confirm/cancel) -> RESULTS (table of metrics, saved status). Sequential stage execution avoids RTOS thread contention; writes /Apps/Data/probe/bench.txt.
2. CPU MHz Metric: 1MHz 64-bit free-running timer @ 0x3c700084 (lo) / 0x3c700080 (hi). Run calibrated ASM decrement loop: N = 10,000,000 iters of unrolled `subs r0,#1; bne` (2 cyc/iter = 20M cycles). Measure elapsed us: `t0 = *0x3c700084; loop(N); dt = *0x3c700084 - t0; MHz = (20000000.0f) / dt`. Precision <0.1% over ~58ms.
3. RAM Bandwidth Metric (Cached vs Uncached): Allocate 2MB buffer in shared heap. Cached: normal virtual/physical pointer (0x08xxxxxx, L1 D-cache active). Uncached: mirror pointer via 0x88000000 base (addr | 0x80000000, MMU 1MB section uncached attribute per swap.md). Measure 16 passes (32MB) of 64-bit NEON (vld1.64/vst1.64) or LDM/STM sequential blocks timed vs 0x3c700084: `MBps = (32 * 1000000.0f) / (dt_us * 1048576.0f)`.
4. GPU Clock & Blitter Metric: (a) Programmed GPU clock derived from CLKCON 0x3c50001c[29:16] Domain 20 divider: `F_gpu = F_PLL / ((div1+1)*(div2+1))` (stock PLL2 266MHz / 2 = 133MHz; verify core ID 0xb105f00d at MBX 0x3b000ff0); (b) Blitter fillrate: submit 100 frames of 240x432 ARGB8888 copies via display pipe @ 0x39000004 (kick bit 0, poll status +0x0c bit 0 done); compute `MPix_sec = (100 * 103680.0f) / dt_us`.
5. Thermals & 30s Stress Metric: Query PMU on I2C1 (0x3c900000) slave 0x73 reg 0x08 before stress (`temp_pre`). Execute 30s saturated ALU/NEON stress loop timed against 0x3c700084; poll PMU reg 0x08 every 500ms to enforce thermal trip point. Read reg 0x08 immediately post-stress (`temp_post`).
6. CPU Boost (CLKCON 0x3c500000) & Safety:
   - Mechanism: Overclock Cortex-A5 from 342MHz to 684MHz by changing PLL1 divider from /2 to /1: write 0x40009000 to 0x3c500000.
   - Safeguards: (a) Modal confirmation screen; (b) Abort and auto-revert to stock 0x40009001 if PMU temp > 65°C; (c) Strict 5s benchmark limit then forced revert to 0x40009001 in all exit paths; (d) Never alter DRAM/Bus dividers (0x3c500004/0x3c50001c); (e) Hang recovery: 8s Sleep+VolDown hardware fallback reboots to stock 240MHz SecureROM.
7. Memory & FS Safety: Avoid SDK file scratch range (0x09118000..0x0911b210); maintain >=2MB heap headroom; write results via single hb_fs_write() buffer.
8. File Output Format (/Apps/Data/probe/bench.txt, Key=Value):
   app=BenchNano_v1
   cpu_stock_mhz=342.00
   cpu_bench_mhz=342.12
   ram_cached_read_mbps=485.20
   ram_cached_write_mbps=320.40
   ram_uncached_read_mbps=112.60
   ram_uncached_write_mbps=94.10
   gpu_clkcon_domain20=0x00010000
   gpu_derived_mhz=133.00
   gpu_blitter_mpix_sec=142.50
   temp_pre_c=24
   temp_post_c=37
   stress_duration_s=30
   boost_applied=0
   boost_bench_mhz=0.00
   status=PASS
