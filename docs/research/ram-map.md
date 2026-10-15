1 MMU table `08b28000–08b2c000` |
| `08b30e5c–08cdc25c` | 1.669 | Secondary heap; control object `0b3e1d38`, singleton slot `0895c530` |
| `08cdc25c–08d184b8` | 0.235 | Outside both examined heaps; ownership unresolved—no safe-steal claim |
| `08d184b8–0b564e80` | 40.299 | Shared main heap: OS objects, caches, stacks, homebrew, capture buffers |
| `0b564e80–0b890f40` | 3.172 | Graphics/video reservation: `2 × 00196060` bytes |
| `0b890f40–0ba5efa0` | 1.805 | Graphics/video reservation: `001ce060` bytes |
| `0ba5efa0–0c000000` | 5.629 | Graphics/video reservation: `005a1060` bytes; contains framebuffers |

**Free subranges—allocate through their owner, never overwrite directly:**

| Snapshot range / aggregate | Size | Evidence |
|---|---:|---|
| Main heap: 167 scattered free blocks | 2.653 | Complete block-header walk; bit 0 marks free |
| `08d188ac–08efd444` | 1.893 | Largest main-heap free block, including allocator overhead |
| `08b30e5c–08b87d10` | 347.676 KiB | Secondary heap’s only free block |
| `0bec3f00–0bf29300`, `0bf29380–0bf8e780` | 405 KiB each | Known framebuffer spans; reserved, overlapping the table above |

The main-heap walk agrees between raw RAM and the overlay applied **in memory**. Zero-filled pages occur inside allocated blocks; this snapshot proves neither “never referenced” nor safe ownership of those pages.

**Enforcement and patch proposal:**
- `[08909134]=089a6228`; `[089a66bc]=0284c9c8` records main-heap managed bytes.
- Initialization: `083fbecc → 08003418 → 08004138 → 084195da`; pool-manager words `08a8d718/08a8d71c` establish the physical ceiling `0c000000`.
- Reservation table `08810814` subtracts `00a9b180` bytes. The heap already reaches its permitted boundary; increasing its accounting fields would overlap graphics memory.
- Allocation: `0842b440 → 0840a03a → 083ff912 → 08009f00`, with size/type arguments and no process quota argument. Homebrew loader `hb_reloc_load` requests `span+align` from this allocator.
- SVC decimal **70** (`0x46`), selector `r12=7`, dispatches to `080fd1a0`: cache maintenance, not heap enlargement.
- **Minimal practical change:** use Thumb entry `0842b441(size,2)` and matched free `0842d86d`; no kernel quota patch is needed. NULL means shared-heap exhaustion.
- **Conditional runtime reclaim experiment:** only after proving reservation’s upper half has no live users, write LE32 `1` at `0881083c` (currently `2`), then call Thumb `084195db(heap,0b6faee0,00196060)` to register that released **1.586 MiB**.
- That experiment is **unvalidated**: active surfaces/video may occupy the slice. Changing the count alone after initialization returns no memory; no unconditional safe RAM-steal patch was established.

tokens used
89,438
**The examined allocator has no 8 MiB per-app quota: homebrew shares a 40.299 MiB OS heap.** Available memory depends on OS allocations and fragmentation.
Addresses below are physical; ranges are **[start,end)**. Sizes are MiB unless stated otherwise.

| Range | Size | Status / owner |
|---|---:|---|
| `08000000–08971704` | 9.443 | Reserved: firmware code, constants, initialized data |
| `08971704–08b30e5c` | 1.748 | Reserved: BSS/kernel/driver tables; includes L1 MMU table `08b28000–08b2c000` |
| `08b30e5c–08cdc25c` | 1.669 | Secondary heap; control object `0b3e1d38`, singleton slot `0895c530` |
| `08cdc25c–08d184b8` | 0.235 | Outside both examined heaps; ownership unresolved—no safe-steal claim |
| `08d184b8–0b564e80` | 40.299 | Shared main heap: OS objects, caches, stacks, homebrew, capture buffers |
| `0b564e80–0b890f40` | 3.172 | Graphics/video reservation: `2 × 00196060` bytes |
| `0b890f40–0ba5efa0` | 1.805 | Graphics/video reservation: `001ce060` bytes |
| `0ba5efa0–0c000000` | 5.629 | Graphics/video reservation: `005a1060` bytes; contains framebuffers |

**Free subranges—allocate through their owner, never overwrite directly:**

| Snapshot range / aggregate | Size | Evidence |
|---|---:|---|
| Main heap: 167 scattered free blocks | 2.653 | Complete block-header walk; bit 0 marks free |
| `08d188ac–08efd444` | 1.893 | Largest main-heap free block, including allocator overhead |
| `08b30e5c–08b87d10` | 347.676 KiB | Secondary heap’s only free block |
| `0bec3f00–0bf29300`, `0bf29380–0bf8e780` | 405 KiB each | Known framebuffer spans; reserved, overlapping the table above |

The main-heap walk agrees between raw RAM and the overlay applied **in memory**. Zero-filled pages occur inside allocated blocks; this snapshot proves neither “never referenced” nor safe ownership of those pages.

**Enforcement and patch proposal:**
- `[08909134]=089a6228`; `[089a66bc]=0284c9c8` records main-heap managed bytes.
- Initialization: `083fbecc → 08003418 → 08004138 → 084195da`; pool-manager words `08a8d718/08a8d71c` establish the physical ceiling `0c000000`.
- Reservation table `08810814` subtracts `00a9b180` bytes. The heap already reaches its permitted boundary; increasing its accounting fields would overlap graphics memory.
- Allocation: `0842b440 → 0840a03a → 083ff912 → 08009f00`, with size/type arguments and no process quota argument. Homebrew loader `hb_reloc_load` requests `span+align` from this allocator.
- SVC decimal **70** (`0x46`), selector `r12=7`, dispatches to `080fd1a0`: cache maintenance, not heap enlargement.
- **Minimal practical change:** use Thumb entry `0842b441(size,2)` and matched free `0842d86d`; no kernel quota patch is needed. NULL means shared-heap exhaustion.
- **Conditional runtime reclaim experiment:** only after proving reservation’s upper half has no live users, write LE32 `1` at `0881083c` (currently `2`), then call Thumb `084195db(heap,0b6faee0,00196060)` to register that released **1.586 MiB**.
- That experiment is **unvalidated**: active surfaces/video may occupy the slice. Changing the count alone after initialization returns no memory; no unconditional safe RAM-steal patch was established.

