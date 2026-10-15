# padfeed.py FIFO -- write a Linux input_event sequence (a gamepad session)
import struct, sys, time
f = open(sys.argv[1], 'wb', buffering=0)
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
def ev(t, c, v): f.write(struct.pack('llHHi', 0, 0, t, c, v))
def frame(*evs):
    for e in evs: ev(*e)
    ev(EV_SYN, 0, 0); time.sleep(0.4)
frame((EV_ABS, 0x00, 32767))                    # left stick right
frame((EV_ABS, 0x00, 1000))                     # inside the flat zone -> 0
frame((EV_KEY, 0x130, 1))                       # A
frame((EV_KEY, 0x130, 0), (EV_ABS, 0x11, -1))   # release A, hat up
frame((EV_ABS, 0x11, 0), (EV_ABS, 0x03, -32768))# right stick left (RX)
frame((EV_ABS, 0x03, 0), (EV_ABS, 0x02, 30000)) # left trigger (Z) -> TL2
frame((EV_ABS, 0x02, -32768), (EV_KEY, 0x13c, 1))  # guide = home
frame((EV_KEY, 0x13c, 0))
frame((EV_ABS, 0x01, -16384))                   # half up
frame((EV_ABS, 0x01, 0))
