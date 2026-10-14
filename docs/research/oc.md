# iPod Nano 7G (S5L8747, Cortex-A5) Clock & Overclocking Analysis
1. PLL/Clock Registers (CLKCON @ 0x3C500000, 24 MHz Fref, Fout = (24M*M)/(P*2^S)):
   - +0x00 CLKCON_CPU: [3:0]=div (val+1), [13:12]=PLL (1=PLL1, 2=PLL2), bit15=gate, bit30=en.
   - +0x04 CLKCON_BUS: [30:25]=AHB div, [22:17]=Field3, [14:9]=Field4, [6:1]=APB div.
   - +0x08 CLKCON_MEDIA: [31:16]=Dom7 (Video 54MHz), [15:0]=Dom8 (Audio I2S 24.18MHz).
   - +0x14 CLKCON_DISP: [31:16]=Dom13 (LCD pixel clock, PLL2 source).
   - +0x1C CLKCON_DRAM: [15:0]=Dom6 (DMC @ 0x38501000, PLL2 source, 133/66.5MHz).
   - +0x20 PLL1_CON (CPU): P=[28:23], M=[12:3], S=[2:0]. Stock: P=6, M=171, S=0 -> 684 MHz.
   - +0x24 PLL2_CON (Bus/DRAM/MBX): P=6, M=133, S=1 -> 266 MHz. +0x28 PLL3_CON: 486/432MHz.
   - +0x30/+0x34/+0x38: Lock time (0x1C20=7200 cyc). +0x40: Lock flags. +0x44: En/bypass.
   - +0x48/+0x4C/+0x58/+0x68/+0x6C: Peripheral clock gates (MBX GPU @ +0x68 bit 17 / dev 46).
2. Stock Snapshot Configuration (dumps/snap6, Fref = 24 MHz):
   - PLL1 = 684.0 MHz (0x03000558). PLL2 = 266.0 MHz (0x03000429).
   - Reg 0x3C500000 = 0x40009001: PLL1 source, div=2 -> Stock CPU = 342.0 MHz.
   - Bus/DRAM (Dom6): PLL2 / 2 = 133.0 MHz (mDDR). MBX GPU (@ 0x3B000000): 133.0 MHz.
3. DVFS Architecture (6 P-States @ IRAM 0x22003884, Handler @ 0x083F7E20):
   - OS state @ 0x0890C520 (+1 = cur P-state 5). Transitions handled via IRAM 0x22000930.
   - Frequencies: P0=6MHz (24M/4), P1=24MHz (24M/1), P2=66.5MHz (PLL2/4), P3=133MHz (PLL2/2),
     P4=266MHz (PLL2/1), P5=342MHz (PLL1/2; snapshot active state).
   - Idle/dim routine @ 0x22000E0E steps down CPU to P3 and gates DRAM/LCD pre-sleep.
4. Safe-Experiment Plan:
   - CPU Bump: PLL1 is ALREADY locked at 684 MHz. Write 0x40009000 to 0x3C500000
     (div 2 -> div 1) for an instant jump from 342 MHz to 684 MHz (+100% clock boost).
   - Alternate: Reprogram PLL1 to 432 MHz (P=6, M=108, S=0) / 532 MHz (P=6, M=133, S=0) + div 1.
   - Verification: Probe app runs SHA1/math loop timed against 1MHz timer @ 0x3C700084.
   - Safeguards: PMU on I2C1 (0x3C900000) slave 0x73 reg 0x08 temp sensor (stock 24°C/0x7D).
     Keep junction temp < 65°C. Leave DRAM/MBX dividers untouched to prevent bus desync.
   - Headroom: 40nm Cortex-A5 nominal ceiling is 800 MHz. 684 MHz runs stable at stock VDD.
5. Risk Assessment:
   - Recoverable: Bad PLL/divider stalls CPU pipeline; holding Sleep+VolDown (8s) triggers
     SecureROM fallback reboot at stock 240 MHz. RAM corruption clears on power cycle.
   - Permanent Damage: PMU core rail (VDD_ARM) overvoltage (>1.25V) causes gate oxide breakdown.
     Sustained high thermals (>75°C) in sealed enclosure risk swelling the adjacent LiPo pouch.
