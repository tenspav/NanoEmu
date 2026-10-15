# Nano 7G Accelerometer & Tilt Steering Architecture (LIS3DH / S5L8747)
1. SDK Subscription & Read Sequence (NanoApps-SDK):
   - API: `int (*accel_sub)(void *desc, void (*cb)(float x, float y, float z)) = (void*)0x0840ff28; void (*accel_en)(int slot, int en) = (void*)0x0840ff6c; void (*accel_unsub)(int slot) = (void*)0x0813c890;`
   - Setup: struct { uint32_t f; void *cb; } d = {0, my_cb}; int slot = accel_sub(&d, my_cb); accel_en(slot, 1); (allocates slot 3..7; slot 2 reserved). Teardown: accel_en(slot, 0); accel_unsub(slot);
   - Direct Polling: Alternatively read global `0x08a91e50` (`int32_t raw[3]`, G = `raw * (1.0f/65536.0f)`) inside game render loop to avoid callback context switching.
2. Steering Filtering & Calibration Math:
   - Center Calib: Average 30-50 samples at game start for baseline `(x0, y0, z0)`; relative tilt `dx = x - x0`.
   - Deadzone: If `|dx| < dz` (dz ≈ 0.04G / ~2.3°), `steer = 0`; else `steer = copysignf((|dx| - dz) / (max_g - dz), dx)`, clamped to [-1.0, 1.0].
   - Smoothing: Low-pass EMA `s[t] = alpha * steer + (1 - alpha) * s[t-1]` with `alpha ≈ 0.20` (~3.5Hz cutoff @ 100Hz) to suppress tremor while keeping responsiveness.
3. Latency & Driving Viability:
   - 100Hz sampler thread = 10ms sampling + ~1.5ms I2C1 transfer + dispatch = ~11-12ms total latency. Oversamples the 60Hz panel (16.7ms frame).
   - ~12ms is well below the 40-50ms perception threshold; delivers near-instantaneous, fluid steering response for driving games.
4. NanoEmu Testing & Emulation Surface:
   - Backend: Hook I2C1 slave 0x18 regfile registers 0x28-0x2D (OUT_X/Y/Z) or inject into global `0x08a91e50` directly on the 100Hz QEMU timer.
   - Input in `nano7_ui.c`: Map A/D or Left/Right keys to smoothly ramp simulated tilt, or RMB-hold + mouse X relative delta for analog steering wheel feel.
5. Pitfalls & Mitigations:
   - Sensor Noise / Taps: Touchscreen taps inject sharp ±0.5G impulses; apply slew-rate limiter `|s[t] - s[t-1]| <= max_delta` and unhook slot 2 to stop shake-to-shuffle music popups.
   - Coordinate Orientation: Device is natively portrait (240x432). In landscape driving mode (432x240), roll steering swaps from X to Y axis, and baseline gravity shifts to ±X.
