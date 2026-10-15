# hidhost -- Bluetooth HID host for gamepads (GL surface kind). SDK: the
# NanoApps tree whose resident is installed on the device (see gltest).
APP_NAME   := hidhost
SRCS       := hidhost.c hh_bt.c hid_parse.c
GL_SURFACE := 1
NANOAPPS   ?= ../../../nanobtfix-untether/nanoapps
include $(NANOAPPS)/sdk/hb_app.mk
