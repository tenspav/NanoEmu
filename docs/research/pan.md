# Bluetooth Internet (BNEP/PAN) & Media Streaming Feasibility (iPod nano 7G)

## 1. Network & Protocol String Search (dumps/snap6/ram-full.bin, base 0x08000000)
- BNEP: 2 hits (0x080fa7fc SDP spec list, 0x0880cf5f CSR BCHS module name). PAN: 123 hits (0x080faa10 SDP PANU/NAP/GN; 0x087fc5f1 Panama locale; no code).
- tether: 2 hits (0x0840e73c/0x0840e748 accessory debug enums). network: 26 hits (DB metadata 'network_name', libc ENETUNREACH, file paths; no stack).
- TCP/IP/socket/HTTP: TCP @ 0x080fa7c4 & IP @ 0x080fa7dc (SDP spec table), TCPasscode... UI classes, 'not a socket' libc errno (0x0880ccac); zero socket APIs.
- DHCP/DNS/TLS/ssl: DHCP @ 0x0853a881 (code byte collision) & 0x08755b0a (build host); DNS/TLS/ssl in OpenSSL libcrypto X.509 cert validation; mbedtls: 0 hits.
- Verdict: NO TCP/IP stack exists in the OS. Kernel is Apple's embedded RTOS ("RetailOS"), lacking all networking, sockets, and IP routing.

## 2. Bluetooth Stack & BNEP/PAN Code Analysis
- Stack: CSR BlueCore Host Software (BCHS) over UART HCI. Supports HCI, L2CAP, RFCOMM, SDP, A2DP Source (0x084e2ea8), AVRCP, and BLE GATT.
- Module table @ 0x087661e0 (index 6 @ 0x08766210 = BNEP, index 18 @ 0x08766270 = PAN) used strictly by debug trace function @ 0x0854c398.
- Code references: ZERO BNEP packet parsing, state machine, or L2CAP PSM 0x000f handling; ZERO PAN SDP service records. Both profiles are dead stubs.

## 3. Feasibility Math: BT 2.1 EDR Throughput vs H.264 Streaming
- BT 2.1 EDR Throughput: 3-DH5 theoretical peak = 2.17 Mbps. Real-world sustained UART HCI throughput = 1.2 - 1.6 Mbps (150 - 200 KB/s).
- Video Bitrates: 144p H.264+AAC = 130-180 kbps (~12% BT bw, 8x headroom); 240p (native 432x240)+AAC = 300-450 kbps (~25% BT bw, 3.5x headroom).
- Streaming Verdict: YES, streamable. Cortex-A5 + Apple M2 decoder easily handle 240p/360p; a 2MB DRAM ring buffer provides ~40s jitter buffer.

## 4. Minimal Path & Optimal Architecture (No OS TCP/IP)
- Minimal Transport: Skip BNEP/lwIP; use existing RFCOMM/SPP channel directly to a companion app on phone/PC (bypasses IP/TLS stack overhead entirely).
- a) Web Browsing: Server/Phone-rendered proxy (Opera Mini style). Phone executes JS/HTML and streams compressed 240w image tiles; nano sends touch inputs.
- b) YouTube Client ('NanoTube'): Phone-side proxy (yt-dlp/Invidious). Phone handles HTTPS/search/remuxing; streams raw 240p H.264+AAC chunks to nano.
- c) Movie Streaming: Direct H.264/AAC bitstream streaming over RFCOMM/SPP into Apple M2 hardware decoder FIFO with ~2MB RAM ring buffer.
