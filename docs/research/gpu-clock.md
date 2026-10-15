S5L8747 MBX GPU & Memory Clock Reverse Engineering Report
1. GPU Clock Register & Divider (Disassembly 0x0810c270 / 0x0842a54e):
   - Register: CLKCON + 0x1c (0x3c50001c), Domain 20 (queried by MBX driver 0x0810c270).
   - Bitfields: [31]=master gate, [30]=clk en, [29:28]=PLL src (0=24M, 1=PLL1, 2=PLL2),
     [23:20]=div2-1, [19:16]=div1-1. GPU Clock = F_PLL / ((div1+1) * (div2+1)).
   - Gates: Un-gated via dev 0x45 (bit 30) & 0x46 (bit 31) at 0x0800578e / 0x0802fa8e.
2. Stock Clock & PLL Headroom:
   - Stock Snapshot (snap6): Domain 20 divider is /2 (div1=1, div2=0) on PLL2 (266 MHz) -> 133 MHz
     (or /6 on PLL1 684 MHz -> 114 MHz, matching active driver timing Entry 2 @ 0x0896e8e0).
   - Max PLL: PLL1 = 684 MHz (0x3c500020), PLL2 = 266 MHz (0x3c500024), PLL3 = 486 MHz (0x3c500028).
3. Divider Cap & Overclock Lift:
   - Yes: div1=0, div2=0 (div /1) lifts the divider cap.
   - On PLL2: div /2 -> /1 bumps GPU from 133 MHz to 266 MHz (+100% boost).
   - On PLL1: src=1, div /4 bumps GPU to 171 MHz (+50% boost; max timing Entry 3 in driver
     table @ 0x08783d44, preventing the 0x084cd0e0 nClockMHz == maxClockMHz panic).
4. DRAM & Bus Clocks:
   - DRAM (Domain 6, 0x3c50001c[15:0]): PLL2 / 4 = 66.5 MHz (133 MT/s mDDR); divider /2 yields 133 MHz.
   - Bus (0x3c500004): Domain 1 (HCLK AHB) = PLL1 / 4 = 171 MHz (or PLL2 / 2 = 133 MHz);
     Domain 2 (PCLK APB) = HCLK / 2 = 85.5 MHz. DVFS idle dims CPU/bus/DRAM to P3 (IRAM 0x220008a8).
5. Safe-Testing & Risks:
   - Safe Test: Modify ONLY 0x3c50001c[29:16] (GPU div). Keep DRAM [15:0] and Bus 0x3c500004 stock to
     prevent bus desync, LCD tearing, or NAND (FMC 0x38a00000) DMA corruption. Start at 171 MHz.
   - Risks: Aluminum unibody has no heatsink; LiPo pouch degrades >65°C (monitor PMU slave 0x73 reg 0x08).
     Pipeline/PLL lock crash recovers via 8s Sleep+VolDown hardware reset to 240 MHz fallback.
