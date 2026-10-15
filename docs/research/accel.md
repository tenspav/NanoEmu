ACCELEROMETER SPEC, iPod nano 7G (Samsung S5L8747); verified from snap6 ram-full.bin / iram.bin.
1. Sensor HW: STMicroelectronics LIS3DH (WHO_AM_I reg 0x0F == 0x33, probe 0x08147ff0; active vtable 0x089177a4 at ptr 0x0890c0c4). Alternate driver: Bosch BMA250 (CHIP_ID reg 0x00 == 0x03, vtable 0x0891781c, probe 0x0814809c).
2. Bus: I2C1 (0x3c900000), 7-bit slave address 0x18. Read: 0x083ff2a8 (registers 0x20-0x33, FIFO 0x2E/0x2F); Write: 0x083dc198 / iram 0x22000652.
3. Driver Task 68 (0x44): "Accelerometer" (string 0x08004080, entry Thumb 0x08006429 / 0x08006428). Periodic timer sampling thread (10ms/100Hz, sleep 0x0842b1dc), reads sensor via 0x0800c5dc.
4. Data Proc Task 69 (0x45): "Accelerometer Data Proc Task" (string 0x08004098, entry Thumb 0x0800641d / 0x0800641c). Waits on RTOS Event 0x47 (0x0842aca8), processes buffer via 0x0800c84c. Event 0x47 posted by 0x0813c698.
5. Sample Staging & Scaling: Raw LE32 counts scaled by 1/65536.0 (float 0x37800000 at 0x0800cc54) into Gs. Last raw sample at 0x08a91e50: X=-2048, Y=-43008, Z=53248 (-0.031G, -0.656G, +0.813G -> ~1.04G gravity vector).
6. App Subscription API: 8-slot subscriber table at 0x08a91e10 ([u32 flags, fn* callback]).
   - Subscribe: 0x0840ff28 (takes client descriptor, installs callback, returns slot 0..7 or -1).
   - Enable/Disable: 0x0840ff6c (slot, enable_flag).
   - Unsubscribe: 0x0813c890 (slot).
7. Sample Delivery: Dispatcher 0x0800cbd0 calls active slots: callback(float x, float y, float z) in r0, r1, r2 (scaled G units).
8. Consumers:
   - Shake-to-shuffle: Slot 2 (descriptor 0x0890c698, callback 0x0813c4f9), computes sqrt(x^2+y^2+z^2), signals shake gesture to HandleShakeShuffleSongsSelected (0x088899e0).
   - Nike+ / Pedometer: Slots 0/1 (callbacks 0x08157ec9, 0x08157eed -> 0x0813c7a4), computes horizontal sqrt(x^2+y^2) for step detection ("%d %f\n", NikeAccel at 0x087fe2aa).
