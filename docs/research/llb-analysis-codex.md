pplied `/home/pavliukov/NanoRework/...` path does not exist.
All 11 containers below are fully covered by successful reads; failed LBAs 5–63 and 47211–49215 remain unknown.
Offsets are absolute in `reserved.bin`; body size is header `+0x10`, total includes the `0x400` header.

| Image | Container offset | Body size | Total size | Type / payload |
|---|---:|---:|---:|---|
| disk | `0x00047000` | `0x000a8413` | `0x000a8813` | 3 / encrypted |
| diag | `0x000f1000` | `0x00200c63` | `0x00201063` | 3 / encrypted |
| fv00 | `0x002f4000` | `0x00011263` | `0x00011663` | 3 / encrypted |
| appl | `0x00307000` | `0x00002f13` | `0x00003313` | 3 / encryption indicated |
| lbat | `0x0030c000` | `0x0002afa3` | `0x0002b3a3` | 3 / encryption indicated |
| bdsw | `0x00339000` | `0x0004cba3` | `0x0004cfa3` | 3 / encryption indicated |
| bdhw | `0x00387000` | `0x0002afa3` | `0x0002b3a3` | 3 / encryption indicated |
| chrg | `0x003b4000` | `0x0002afa3` | `0x0002b3a3` | 3 / encryption indicated |
| gpfw | `0x003e1000` | `0x0000f5c3` | `0x0000f9c3` | 3 / encryption indicated |
| rsrc | `0x003f2000` | `0x0ab00c63` | `0x0ab01063` | 3 / plaintext FAT16 payload |
| osos | `0x0aef5000` | `0x00974dc3` | `0x009751c3` | 3 / encrypted |

Directory: `0x45000`, eleven 40-byte records. No additional valid container or identified hibernate image; other `8740` matches are certificate text/XML.
**LLB is absent from this logical dump.** IPSW `N31.bootloader.rb3` is Type 3, payload `0x23800`, total `0x24863`; ROM entry target is `0x22000000`.
Its matching header survives at IRAM `0x22025000`, but OS code has overwritten the body: a reliable LLB disassembly requires decrypted/raw bootstrap acquisition.
**Selector correction:** readable `snap6/ram-full.bin` proves selection in UEFI `BdsN31`, rather than establishing it in LLB.
At runtime `0x0bc2e56c`, AppleButton IDs 0,1,2 are queried; all pressed selects diagnostics at `0x0bc2e5ec`. Physical VolDown/Sleep mapping remains unverified.
Diag calls `0x0bc2f488` → `0x0bc2fc98`, trying `FV/FVDIAG.Fv`, host `FVDIAG.Fv`, then `diag`; normal boot uses `osos` at `0x0bc2ec6c`.
**Type 4 is accepted by the readable UEFI validator** at `0x0bc789c0–c8`; plaintext does **not** bypass its signature checks.
Header validation `0x0bc789a4`: magic, type, SHA-1 over header `[0,0x40)`, AES processing and comparison against the 16-byte tag at `+0x40`.
Body validation `0x0bc78658`: aligned payload length P; body ≥ P+`0x300`; certificate offset ≥ P+`0x80`, 16-byte alignment; entry ≤ P.
It hashes header+P bytes, then calls certificate/RSA verification at `0x0bc78714`; rejection precedes the Type-3-only AES branch at `0x0bc7876c`.
Verifier `0x0bc77d78` → `0x0bc77a50` checks a pinned certificate digest, CA identity and chain signatures; `0x0bc77bbc` checks RSA PKCS#1 v1.5 padding/digest.
Diag payload: `0x000f1400`, length `0x200000`, declared load `0x08000000`; signature `0x002f1400` (128 bytes), certificates `0x002f1480` (length `0xbe3`).
LLB-specific enforcement and any caller bypass remain unproven; the generic validator evidence supports authenticated diagnostics, not unsigned Type-4 replacement.
Readable now: directory, headers, signatures/certificates, rsrc FAT16 at `0x003f2400`, and surviving UEFI code. Executable ciphertext needs device AES access; decryption grants no signing ability.
**Low risk:** prototype button-selected chainloading through existing OS homebrew; this requires OS boot and is not the cold diagnostics path.
**Medium risk:** demonstrate a volatile pre-BDS validator/selector hook, if an early execution primitive exists; none is established here.
**High risk:** persist a diag replacement only after proving an authentication bypass, allocation bounds, supported writes and recovery; a signed-image parser exploit is another unproven route.
**Very high risk:** modify LLB/bootstrap replicas; BootROM authentication must also be overcome. Valid trusted signing would avoid a bypass, but no signing key is available.
