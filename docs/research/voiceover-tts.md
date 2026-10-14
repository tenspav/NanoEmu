# iPod nano 7G (S5L8747) VoiceOver Architecture & Homebrew Speech Synthesis Analysis
1. Mechanism: Hybrid embedded TTS + Pre-rendered audio fallback
   - Embedded On-Device TTS: Nuance RealSpeak / Vocalizer is fully embedded in firmware (0x0832ee00-0x08383000, BET2 v5.0/v5.20, TTSEG pipeline). Voice definitions reside in /Resources/Vocalizer with 29 supported languages in table 0x088115f4 (default: Lang 0, id=7, Samantha, en-US).
   - Spoken Clips / Pre-Rendered Fallback: Handled by TSpeakablePlayback (task @ 0x08056338) and TSpeakableScheduler; checks /iPod_Control/Speakable and /Resources/Speakable for pre-generated .aifc clips. FAT32 scan at offset 258048 confirms NO Speakable clips on disk; speech is dynamically synthesised on-device. UI earcons are raw WAVs (/Resources/Sounds/VoiceOver_*.wav).
2. Key Function & Thread Addresses:
   - TTS Task Thread: 0x08169238 (Thumb; created at 0x0813fb28, stack 0x8000, priority 9; message loop processing opcodes 0=Init/Voice, 1=Halt, 2=Speak, 3=Sync).
   - TTS Speak Text API: 0x08161d28 (Thumb; prototype: void tts_speak(const char *text, uint32_t len, uint32_t lang_id, uint32_t flags); allocates 12B descriptor + text copy, enqueues opcode 2 msg to mailbox 0x08990530).
   - Voice & Language Selection: 0x0818ed88 (auto-detects language), 0x08073d10 (getter for 0x088115f4 table entries).
   - Engine Init & Resource Loader: 0x0815bf1c (resolves /Resources/Vocalizer paths via 0x0814fdf8 and configures RealSpeak instance).
   - Vocalizer Core Routines (ARM): 0x0832eef8 (engine init), 0x083552b4 (TTSEG Speak), 0x08354b2c (TTSEG Synthesize), 0x08354ecc (TTSEG SynthesizeCooperatively).
   - Audio Sink Callback: 0x0815bcdc (receives raw L16 PCM buffers from TTS engine to pipe into OS audio system).
   - Pre-rendered Audio Task: 0x08056338 (TSpeakablePlayback thread entry).
3. Audio Output Path:
   - Synthesised PCM (audio/L16) routes from TTS sink callback (0x0815bcdc) into OS MeCCA audio mixer (MeCCAOutputTask, Thumb entry 0x080a6ec6).
   - The mixer streams PCM via DMA (engine 1 @ 0x38200000 / engine 2 @ 0x38700000, peripheral req 12) into S5L8747 I2S0 TX FIFO (0x3ca00010) -> Cirrus CS42L81 codec over SPI0 (0x3c300000).
4. How Homebrew Apps Can Speak Custom Text:
   - Direct Call: Cast 0x08161d29 (Thumb) to `void (*)(const char *str, uint32_t len, uint32_t lang, uint32_t flags)` and call: ((void (*)(const char*, uint32_t, uint32_t, uint32_t))0x08161d29)("Hello Nano", 10, 7, 0); (lang 7 = US English Samantha, 6 = British English Daniel).
   - Alternatively, allocate an 8-byte msg header [0]=0x02, [1]=ptr_to_descriptor and post to OS message queue at 0x08990530 via 0x083f6260.
