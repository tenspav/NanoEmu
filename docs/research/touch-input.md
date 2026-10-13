```text
TOUCH SPEC — verified from dumps/snap2/ram-full.bin; code addresses are Thumb entry addresses.
1. Driver/transport
MultitouchTask: thread 11, entry 0x080eec30; IRQ callback 0x08163ab0; event 0x43.
Touch transport 0x08400430 -> 0x083f4ee8 uses synchronous serial port 2 at 0x3d200000.
Registers: 0x3d200000 control; 0x3d200004 transfer control; 0x3d200008 status/W1C.
Registers: 0x3d20000c chip-select bit 1; 0x3d200010 TX; 0x3d200020 RX.
Registers: 0x3d200034 RX count; 0x3d20004c TX count.
IRQ11 entry 0x08000c94 -> handler 0x08000fec; completion wakes event 0x44.
Touch-ready GPIO 0x26 = bank 4, pin 6; level register 0x3cf00084, bit 6.
GPIO IRQ32: pending 0x397000a4, mask 0x397000c4, configuration 0x39700084, bit 30.
IRQ32 entry 0x08000912 -> 0x083a33be -> 0x08163ab0; pending acknowledged by writeback.
2. Packet/data
Length query 0x0817db98: transmit 16 bytes beginning EA 01 00; final LE16=sum(first 14 bytes).
Query reply: LE16 length at byte 1; LE16 checksum at byte 14=sum(first 14 bytes).
Fetch 0x08179b18: transmit EA 01 01; transfer returned length+5 bytes.
RX buffer 0x08a97400; TX buffer 0x08a97600; maximum transfer 512 bytes.
RX header: EA, byte 1, LE16 payload-length-including-checksum, header-checksum byte.
sum(header bytes 0..4)&255 must equal 0; final LE16=sum(payload excluding checksum).
Contact parser 0x081832cc: payload[0]=0x44; payload[2]=contact-record offset.
payload[0x10]=contact count; payload[0x11]=record stride.
Record: byte 0 contact ID; byte 1 state; byte 2 type; signed LE16 X/Y at +4/+6.
New contact requires state 3 or 4 and type 1..5; state 4 moves; state >=5 releases.
Contact slots 0x08a973d0+6*n: active byte +0, ID byte +1; eight slots.
Conversion: x=trunc((rawX+75)*239/2845); y=431-trunc((rawY+75)*431/5250).
Display descriptor 0x08741a7c explicitly stores width 240, height 432 at +0x0c/+0x10.
240x320 scaling is therefore unverified; this snapshot's input/display dimensions are 240x432.
3. Consumers/queues
Thread 11 parses packets and calls 0x08143ba4 -> 0x084046a8.
Per-contact queues: 0x089a5298+24*n; coordinates also staged at 0x089a5210+8*n.
Queued record: +0 ID(u32), +4 kind(u8: 0 down, 1 up, 2 move), +8/+0x0c X/Y(s32).
Aggregate touch notification: 0x08173eb4 -> 0x08418860 -> 0x0801a344.
Queue pointer 0x089090d8 = 0x0b432d20 in snapshot; insertion ring at 0x0b432d2c.
4. Buttons
Volume-up GPIO 0x28: 0x3cf000a4 bit 0; volume-down GPIO 0x29: same register bit 1; active-low.
IRQ31 pending/mask 0x397000a8/0x397000c8 bits 0/1; callback 0x081541f4.
GPIOButtonManagerTask thread 20, entry 0x080de2cc; debounce 10000 us; emits through 0x08408570.
Home/power use I2C1 0x3c900000, slave 0x73, register 0x07 bits 4/5, active-low.
Home/power sampling 0x083a6f80; dispatch 0x080fb270; GPIO 0x56 wakes the shared status path.
5. Minimum mouse path
Implement serial TX/RX/counts/status/IRQ11 plus GPIO 0x26/IRQ32; supply checksummed down then release reports.
Use matching contact ID/type, state 3 then 5; invert the conversion above for mouse coordinates.
Normal input initialization must enable 0x08909068[0]; it is zero in this snapshot; tap delivery was not runtime-tested.
```

