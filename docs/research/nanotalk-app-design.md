# NanoTalk: iPod nano 7G Homebrew TTS Application Design
1. Overview: Demonstrates Nuance RealSpeak TTS via direct call 0x08161d29 (Thumb) to synthesize speech through MeCCA mixer to I2S.
2. UI Layout (LVGL + hb_sdk): 240x432 screen with three action buttons and a T9 text field:
   - [Speak Time]: reads RTC (hb_time_get / hb_rtc), formats "It is HH:MM", calls tts_speak.
   - [Speak Menu]: speaks static UI label ("Settings, VoiceOver enabled", Lang 7 Samantha / Lang 6 Daniel).
   - [Speak Input]: reads string from lv_textarea populated by hb_t9 modal keyboard, speaks custom text.
3. Calling Sequence & Safety Checks:
   - Check mailbox: verify *(volatile uint32_t*)0x08990530 != 0; if null/uninitialized, invoke TTS task spawn at 0x0813fb29().
   - Sanitization: ensure text != NULL && len > 0 && len < 256 (avoids heap overflow in RealSpeak 12B descriptor alloc).
   - Dispatch: ((void(*)(const char*, uint32_t, uint32_t, uint32_t))0x08161d29)(text, len, lang_id, 0); (lang 7=en-US, 6=en-GB).
   - Async execution: call is non-blocking; posts opcode 2 to queue 0x08990530; RealSpeak BET2 engine streams L16 PCM to mixer.
4. NanoEmu Test Plan:
   - Launch: ./emulator/run.sh --app /Apps/NanoTalk.app --audio-out /tmp/nanotalk.wav
   - Stimulus: simulate touch events at button coordinates (or via test harness) to trigger time, menu, and custom T9 speech.
   - Verification: inspect emulator exit summary (audio duration > 0s, peak level > 0 dB) and verify /tmp/nanotalk.wav PCM.
