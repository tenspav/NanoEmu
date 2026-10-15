# TiltLab: iPod Nano 7G Accelerometer Calibration & Ball Motion Demo
App Structure: Raw-surface SDK app (`hb_raw_init`/`frame`, 240x432, `hb_app.mk` as in `probe/dumpram/`).
Exact Subscription Code Path:
  typedef void (*accel_cb_t)(float x, float y, float z);
  int (*sub)(void *d, accel_cb_t cb) = (void*)0x0840ff28; void (*en)(int s, int e) = (void*)0x0840ff6c; void (*unsub)(int s) = (void*)0x0813c890;
  struct { uint32_t f; void *cb; } d = {0, my_cb}; int slot = sub(&d, my_cb); en(slot, 1);
  // Exit: en(slot, 0); unsub(slot); (Alternatively poll raw LE32 counts at 0x08a91e50 * 1/65536.0f).
Ball Physics & G-to-Velocity Math:
  - Neutral/Deadzone: dG = G - G_neutral; G_eff = (|dG| < 0.04f) ? 0.0f : copysignf((|dG|-0.04f)/(1.0f-0.04f), dG); slew-limit |ΔG| <= 0.2G (tap reject).
  - Filter: EMA G_filt = α * G_eff + (1 - α) * G_filt (toggle: α = 0.20 EMA, α = 1.0 Off).
  - Integration: vel = (vel + G_filt * k_accel * dt) * friction; pos += vel * dt; bounce clamp (vel = -e * vel).
Calibration & Jitter Metric:
  - "Set Neutral" button snapshots G_neutral = (Gx, Gy, Gz) baseline offset.
  - Jitter metric: N=100 sample ring buffer at rest; stddev σ = sqrt(Σ(G_i - μ)² / N) in milli-G.
NanoEmu Testing (Fake Accel):
  - In `nano7_ui.c`: map Arrow/WASD keys to ramp target tilt angles (Gx, Gy, Gz) on key_event.
  - Emulation surface: hook I2C1 slave 0x18 regs 0x28-0x2D (LIS3DH OUT) or inject into `0x08a91e50` at 100Hz timer.
