ay transmitter, Bluetooth touch debugging, and a scripted test reader.** Addresses are snapshot addresses; findings below are ranked by homebrew value. No files written.

1. **Digital display transmitter, `0x3e600000` (strong inference).** Dominant unlisted block: **160 candidate literal-load sites** in the main firmware.
   Driver `0x080d4ebc-0x080d6460` programs video timing, interrupt/status banks, and a transaction engine around `+0x790`.
   Caller `0x0813ef3c` reads slave `0x50` in 128-byte chunks and checks byte-sum checksums: compelling **EDID** evidence.
   **DisplayPort/AUX is the leading identification**; digital output support is clear, but accessible routing and exact interface remain unproved.

2. **Bluetooth touch-debug service (strong software evidence).** `Advertising Touch Debug Profile` at `0x082c2060`, `ServiceTouchDebug` at `0x082c4b70`.
   `_touch_debug_bt` at `0x0880855f` appears in the preference table at `0x087547b8`, ID **`0xa9`**.
   Potential wireless touch inspection/injection interface; packet format and enabled state still need tracing.

3. **Scripted test reader (confirmed code path; scope uncertain).** `commands.txt` at `0x084a1690` is referenced by ADR at `0x084a131e`.
   Nearby code parses text/numbers and branches through several operations; `ENDTEST1`, `Recordings/`, and `.wav` accompany it.
   Promising automation hook, probably a recording/media test harness. **No general interactive shell established.**

4. **Bluetooth engineering mode and logs.** `_enable_bt_test_mode` at `0x08807ae7`; startup explicitly checks for a test-mode underscore file (`0x08767e1c`).
   Symbols include LE transmit/receive tests and vendor-command dispatch (`0x08767132-0x08767373`).
   Capture path: `iPod_Control/BTLogs/BT_hci.pklg` at `0x0880d4b6`. Presence does not establish current activation.

5. **Serial diagnostics switches.** `_touch_rawdata_to_uart` (`0x088085d1`), `_tpodlogprint_to_uart` (`0x088085e8`), `_uart30pin_debug` (`0x088080be`).
   These are concrete leads for touch capture and firmware logging; physical endpoint selection remains unresolved.

6. **Remaining four highest-count unlisted MMIO bases**, after excluding known-subsystem aliases and obvious numeric/data false positives:
   **`0x3dd00000`, 12 RAM literal-load sites:** additional UART; RX/status/TX registers and baud divisor at `+0x28`, initialized at `0x08550b44`. IRAM adds nine literal-load sites.
   **`0x3cc00000`, nine RAM immediate-load sites:** UART sibling selected by the same driver; FIFO handling at `0x08001868`.
   **`0x3dc00000`, nine RAM immediate-load sites:** another UART sibling, same register layout and initialization path.
   **`0x3e000000`, five RAM immediate-load sites:** system/multimedia control glue; sleep code changes `+8/+0xc`, video paths change `+4`. No independent accelerator demonstrated.
   Counts describe inspected static load sites, **not runtime accesses or a complete reconstructed call graph**.

7. **Hardware random-word generator candidate, `0x3c100000`.** Reader `0x0809e364` polls status at `+0`, returns a word from `+4`.
   Initializer `0x08423d4c` seeds `+8` using time/date-derived values and discards initial outputs. **Hardware PRNG plausible; true entropy unproved.**

8. **Accessory expansion hooks.** RTTI includes `CIapLingoLocation` (`0x0878ccd5`), `CIapLingoDigitalAudio` (`0x0878d71b`), and `CIapLingoMicrophone` (`0x0878d1a8`).
   External location/audio accessories are plausible targets; these do **not** demonstrate onboard GPS or a microphone.

9. **Other useful software:** BLE heart-rate datasource (`0x0846fa20`), AES-encrypted Bluetooth keychain serialization (`0x0842e794`), and FairPlay verify/compute paths (`0x0865d85c`).
   `keyBag` and “payment gateway” matches belong to generic certificate-library vocabulary; no secure element/payment feature established.
   No convincing new coprocessor, SHA-2 hardware, NFC, compass, Siri/dictation, or haptics hardware emerged.
