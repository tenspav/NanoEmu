# NAND Storage Memory Extension & Streaming Feasibility (iPod nano 7G)

## 1. Hardware & NAND Architecture
- S5L8747 FMC (0x38a00000) with FMSS sequencer (+0xc00, IRQ 54) drives Apple PPN 1.0 NAND via DMA to DRAM.
- Geometry: 16 KB pages (4x4KB sectors), 2 CEs, 2 planes; 16-byte sftl metadata per sector. GPU (MBX Lite) shares 64MB DRAM.

## 2. Realistic NAND Throughput & Latency
- Read Latency: ~50-75 us cell sense (tR) + ~400 us DMA transfer (16KB @ ~40 MB/s bus) = ~0.5 ms per 16 KB page.
- Throughput: ~25-35 MB/s sequential read across CEs; sustained write reaches ~16 MB/s (measured: 64MB RAM dump in 4.11 s).

## 3. OS Virtual Memory & mmap Investigation (dumps/snap6/ram-full.bin)
- Kernel: Single-address-space RTOS ("RetailOS"), not Darwin/XNU. No POSIX mmap, madvise, or pager/swap subsystems exist.
- Strings: 'mmap' occurs 3x at 0x0bf8e780 (stale 48-byte heap descriptor in high DRAM, later clobbered by UI pixels); zero in code.
- 'swap' = UI transitions (HandleSwapToNowPlaying); 'pager' = vCard pagerTelephoneNumber; 'vm_*' = 0 hits.
- MMU (TTB @ 0x08b28000): 100% 1MB Section mappings (flat DRAM, IRAM, MMIO, 0x88000000 uncached mirror). No 4KB coarse tables.

## 4. Engineering Verdict & Recommended Architecture
- Classic Swap (IMPOSSIBLE): No demand-paging/fault handler in OS. Latency math: 16KB fault costs ~0.5-2 ms (3-12% of 16.6 ms frame; 10 faults = 15 ms stall). Eviction writes (tPROG ~1.5 ms) and erase cycles would rapidly burn MLC NAND.
- mmap Texture Streaming: Unviable via OS (no kernel mmap/page cache), but HIGHLY VIABLE via user-space streaming.
- Optimal Architecture (Open World/Big Game): App-level async asset streaming engine. Dedicated 16-24MB DRAM pool; worker thread DMA-reads sequential 16KB/64KB chunks into ring buffers directly to unified DRAM for MBX Lite, masked by spatial prefetching.
