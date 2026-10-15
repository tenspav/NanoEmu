# Makefile for bare-metal hello payload (iPod nano 7G)
# Produces raw binary payload.bin linked to run at physical DRAM 0x08000000.

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size

CFLAGS  := -mcpu=cortex-a5 -marm \
           -fno-pic -fno-builtin -ffreestanding -nostdlib \
           -fno-jump-tables -fno-common -fno-exceptions \
           -O2 -Wall -Wextra \
           -fdata-sections -ffunction-sections

LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,--build-id=none \
           -Wl,-T,payload.ld

SRCS    := start.S payload_hello.c
OBJS    := start.o payload_hello.o

BUILD   := build
TARGET  := payload.bin
ELF     := $(BUILD)/payload.elf
BIN     := $(BUILD)/payload.bin

.PHONY: all clean inspect

all: $(TARGET)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(ELF): $(addprefix $(BUILD)/,$(OBJS)) payload.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(addprefix $(BUILD)/,$(OBJS))
	@$(SIZE) $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(TARGET): $(BIN)
	cp $< $@
	@echo "=== $@ ==="
	@wc -c $@

inspect: $(ELF)
	arm-none-eabi-objdump -d $<

clean:
	rm -rf $(BUILD) $(TARGET)

# --- piece 2: hw_shutdown_all() compile check (PIECE2.md) -------------------
# shutdown.c is built with the SDK app flags (Thumb, as hb_app.mk: it runs in
# the launcher app's process) and then linked ALONE with -nostdlib + libgcc
# and --no-undefined: any OS / SDK / libc reference fails the build.
NM       := arm-none-eabi-nm
NANOAPPS ?= $(firstword $(wildcard ../../../nanobtfix-untether/nanoapps /home/pavliukov/ipodwork/NanoApps))
SD_CFLAGS := -mcpu=cortex-a8 -mthumb -mfpu=neon \
             -fno-pic -fno-builtin -ffreestanding -nostdlib \
             -fno-jump-tables -fno-common -fno-exceptions \
             -Os -Wall -Wextra -Werror -fdata-sections -ffunction-sections \
             -fno-strict-aliasing -I$(NANOAPPS)/sdk -I$(NANOAPPS)/sdk/freestanding
SD_LIBGCC := $(shell $(CC) -mcpu=cortex-a8 -mthumb -mfpu=neon -print-libgcc-file-name)

.PHONY: shutdown-check
all: shutdown-check
shutdown-check: $(BUILD)/shutdown-check.elf

$(BUILD)/shutdown-app.o: shutdown.c shutdown.h | $(BUILD)
	$(CC) $(SD_CFLAGS) -c $< -o $@

$(BUILD)/shutdown-check.elf: $(BUILD)/shutdown-app.o
	$(CC) $(SD_CFLAGS) -Wl,--gc-sections -Wl,--build-id=none \
	    -Wl,-e,hw_shutdown_all -Wl,-Ttext=0x09280000 -Wl,--no-undefined \
	    $< $(SD_LIBGCC) -o $@
	@if $(NM) -u $@ | grep -q .; then \
	    echo "shutdown.c has undefined symbols:"; $(NM) -u $@; exit 1; fi
	@$(SIZE) $@

# --- piece 3: handoff_to_payload() compile check (PIECE3.md) ----------------
.PHONY: handoff-check
all: handoff-check
handoff-check: $(BUILD)/handoff-check.elf

$(BUILD)/handoff.o: handoff.S handoff.h $(TARGET) | $(BUILD)
	$(CC) $(SD_CFLAGS) -c $< -o $@

$(BUILD)/handoff-check.elf: $(BUILD)/handoff.o
	$(CC) $(SD_CFLAGS) -Wl,--gc-sections -Wl,--build-id=none \
	    -Wl,-e,handoff_to_payload -Wl,-Ttext=0x09280000 -Wl,--no-undefined \
	    $< -o $@
	@if $(NM) -u $@ | grep -q .; then \
	    echo "handoff.S has undefined symbols:"; $(NM) -u $@; exit 1; fi
	@$(SIZE) $@

# --- piece 4: chainloader app glue (chainloader.elf / .bin / .hbapp) -------
MKRELOCAPP := $(firstword $(wildcard $(NANOAPPS)/tools/mkrelocapp.py /home/pavliukov/ipodwork/NanoApps/tools/mkrelocapp.py))

.PHONY: chainloader
all: chainloader
chainloader: $(BUILD)/chainloader.elf $(BUILD)/chainloader.bin $(if $(MKRELOCAPP),$(BUILD)/chainloader.hbapp)

$(BUILD)/hb_fs.o: $(NANOAPPS)/sdk/hb_fs.c | $(BUILD)
	$(CC) $(SD_CFLAGS) -c $< -o $@

$(BUILD)/chainloader-app.o: chainloader.c shutdown.h handoff.h | $(BUILD)
	$(CC) $(SD_CFLAGS) -c $< -o $@

$(BUILD)/chainloader.elf: $(BUILD)/chainloader-app.o $(BUILD)/shutdown-app.o $(BUILD)/handoff.o $(BUILD)/hb_fs.o
	$(CC) $(SD_CFLAGS) -Wl,--gc-sections -Wl,--build-id=none \
	    -Wl,-e,payload_entry -Wl,-Ttext=0x09280000 -Wl,--no-undefined \
	    $^ $(SD_LIBGCC) -o $@
	@if $(NM) -u $@ | grep -q .; then \
	    echo "chainloader has undefined symbols:"; $(NM) -u $@; exit 1; fi
	@$(SIZE) $@

$(BUILD)/chainloader.bin: $(BUILD)/chainloader.elf
	$(OBJCOPY) -O binary $< $@
	@echo "=== $@ ==="
	@wc -c $@

$(BUILD)/chainloader.reloc.elf: $(BUILD)/chainloader-app.o $(BUILD)/shutdown-app.o $(BUILD)/handoff.o $(BUILD)/hb_fs.o
	$(CC) $(SD_CFLAGS) -Wl,--gc-sections -Wl,--build-id=none \
	    -Wl,-q -Wl,-e,payload_entry -Wl,-Ttext,0 \
	    $^ $(SD_LIBGCC) -o $@

$(BUILD)/chainloader.hbapp: $(BUILD)/chainloader.reloc.elf
	python3 $(MKRELOCAPP) $< $@ payload_entry
	@echo "=== $@ ==="
	@wc -c $@
