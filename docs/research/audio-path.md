tors contain **SRC,DST,next,config,count** (five words).
Completion: engine **`+0x04` bit n**, cleared by **`+0x08` bit n**; shared ISR **`0x220010ee`** invokes the channel event.
4. **Output thread:** **`MeCCAOutputTask`**, Thumb entry **`0x080a6ec7`**; worker **`0x080b341c`**, mixer callback **`0x0809c5b8`**.
Constructor **`0x080a2060`** creates input/output tasks through `0x083e3bda`; output requests **`0x1000` bytes / `0x400` units** per iteration.
Lazy backend getter **`0x083e2090`**, selected by **`0x08414d18`**; opening Music specifically is not established as the trigger.
5. **Minimum emulation:** I2S register readback/reset completion; paced DMA transfers to `0x3ca00010`, linked descriptors, counts, IRQ/event completion.
Implement SPI FIFO/transaction completion and the codec’s 16-bit register protocol, retained register values, volume/routing writes, and `0x002f` completion bit.
Preserve output-thread wakeups and timer progress; silent sample consumption can satisfy the driver before host sound is implemented.
No files written; DMA ISR inspection additionally used the existing `snap6/iram.bin`.
tokens used
268,951
Snapshot-derived spec; addresses below are code addresses (Thumb function pointers add 1). Physical codec part name and dedicated I2S IRQ remain unverified.
1. **I2S blocks:** selector `0/1/2` → `0x3ca00000/0x3cd00000/0x3d400000`; getter `0x08427b08`.
Built-in output uses selector **0**, TX FIFO **`0x3ca00010`**; constructed at `0x080a2138`, submitted at `0x080b346e`.
Per-block registers: `+0x00` reset/control; `+0x04` format; `+0x08` TX control; `+0x10` TX FIFO.
Also `+0x34` RX control, `+0x38` RX FIFO, `+0x3c` status/W1C, `+0x40` clock divisor.
Format routine `0x080ba31c`; TX start `0x080b3ee8` ORs `+0x08` with `6`; stop `0x0854faf4` clears bit `4`.
Reset routine `0x080bdf38`: writes `+0x00=1`; shutdown clears controls and waits for `+0x00` bit `1` to set.
Divisor writer `0x0804ef86`; rate routine `0x080d077c`: 44,100 Hz → `+0x40=0x110`, 48,000 Hz → `0xfa` (subject to clock selection).
Status helpers `0x080bdec8/0x080bdeea` test/clear `+0x3c` bit `15+direction`.
No dedicated I2S IRQ/ISR identified; sample completion uses DMA IRQs below.
2. **Codec control is SPI, not I2C:** selected firmware codec ID **`0x4281`**, dispatch table `0x0890c3d0`, selected pointer `0x0890bc68`.
SPI controller **0**, MMIO **`0x3c300000`**; chip select controlled through `+0x0c` bit `1`, helper `0x083f50ec`.
Register write `0x08429fb8`: bytes **`6c reg_hi reg_lo 00 value`**; read `0x08429ffe`: **`6d reg_hi reg_lo 00 dummy`**.
Stereo consecutive write `0x083eab80`: `6c reg_hi (reg_lo|80) 01 value value`.
Volume writer `0x083b6a4c`: output-selector 0 uses **`0x0227/0x0228`**, selector 1 uses **`0x0229/0x022a`**.
Codec init/power routine `0x080d053c`: unlock **`0x9901←a5`, then `00`** via `0x080d01b8`.
Playback-state branch writes **`0xc81f←ff`, `0xc85f←0f`, `0xc96f←0e`, `0x0223←08`, `0x0224←09`**.
Power/mute control includes **`0x0006`, `0x0007`, `0x000e`, `0x0220`**; routing routine `0x080d0220` also updates **`0x0203–0x0207`**.
Rate setup updates **`0x000f`, `0x012f`, `0x0130/0x0131`, `0x0222`**; 44.1-kHz rate code is **`0x0a`**.
Shutdown at `0x080d073e` polls **codec `0x002f & 0x80`** indefinitely; emulate that completion.
SPI ISR **`0x08000fec`**, controller-0 IRQ **9**; `+0x08` status/W1C, `+0x10` TX, `+0x20` RX, completion wakes event `0x44`.
3. **DMA:** runtime allocation, not a proven fixed channel; allocator `0x080a309c`, output peripheral request **12** → **`0x3ca00010`**.
Driver engine ID `1` → **`0x38200000`, IRQ 17**; ID `2` → **`0x38700000`, IRQ 16**; eight channels each.
Channel registers: `base+0x100+0x20*n`: **SRC `+0`, DST `+4`, link `+8`, config `+0xc`, control `+0x10`, count `+0x14`**.
Submission `0x080b1cb4` writes control including **`0x8001`**; linked descriptors contain **SRC,DST,next,config,count** (five words).
Completion: engine **`+0x04` bit n**, cleared by **`+0x08` bit n**; shared ISR **`0x220010ee`** invokes the channel event.
4. **Output thread:** **`MeCCAOutputTask`**, Thumb entry **`0x080a6ec7`**; worker **`0x080b341c`**, mixer callback **`0x0809c5b8`**.
Constructor **`0x080a2060`** creates input/output tasks through `0x083e3bda`; output requests **`0x1000` bytes / `0x400` units** per iteration.
Lazy backend getter **`0x083e2090`**, selected by **`0x08414d18`**; opening Music specifically is not established as the trigger.
5. **Minimum emulation:** I2S register readback/reset completion; paced DMA transfers to `0x3ca00010`, linked descriptors, counts, IRQ/event completion.
Implement SPI FIFO/transaction completion and the codec’s 16-bit register protocol, retained register values, volume/routing writes, and `0x002f` completion bit.
Preserve output-thread wakeups and timer progress; silent sample consumption can satisfy the driver before host sound is implemented.
No files written; DMA ISR inspection additionally used the existing `snap6/iram.bin`.
