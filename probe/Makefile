APP_NAME := probe
SRCS     := probe.c
RELOC    := 1
include /home/pavliukov/ipodwork/NanoApps/sdk/hb_app.mk

# deliver the blob at the repo root of this probe dir as probe.hbapp
all: probe.hbapp
probe.hbapp: $(APP_OUT)
	cp $(APP_OUT) $@
	@wc -c $@

clean: clean-hbapp
clean-hbapp:
	rm -f probe.hbapp
