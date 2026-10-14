# gltest -- minimal OpenGL ES 1.1 test app (GL surface kind) for bringing up
# the MBX GPU model in the emulator. SDK: the NanoApps tree whose resident
# is installed on the device (snap6: nanobtfix-untether; its Blocks.hbapp
# is byte-identical to the one on the disk image).
APP_NAME   := gltest
SRCS       := gltest.c
GL_SURFACE := 1
VARIANT    ?= 0
EXTRA_CFLAGS += -DVARIANT=$(VARIANT)
NANOAPPS   ?= ../../../nanobtfix-untether/nanoapps
include $(NANOAPPS)/sdk/hb_app.mk
