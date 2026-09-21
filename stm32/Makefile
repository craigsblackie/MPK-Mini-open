# Two images are built here, and they are flashed differently.
#
#   mpk-mini-open      the application. Lives in the slot at 0x08008000,
#                      outside anything the stock AKAI updater inspects,
#                      and is what the ESP32's web editor uploads.
#
#   mpk-mini-resident  the loader plus the recovery application. Lives at
#                      0x08002000, is what the stock updater checksums and
#                      jumps to, and is installed once over SWD. Its final
#                      halfword is patched so the stock updater's additive
#                      checksum over 0x08002000..0x080077ff is zero.
#
# See include/otamap.h for the layout and README.md for the flashing order.

# One source of truth for both firmwares' version, shared with the ESP32
# project next door. Without this the reported version came from whatever
# git tree happened to be around the build, which said more about the
# working copy than about the firmware.
VERSION_FILE = ../version.txt
VERSION := $(shell cat $(VERSION_FILE) 2>/dev/null || echo 0.0.0)
VERSION_MAJOR := $(word 1,$(subst ., ,$(VERSION)))
VERSION_MINOR := $(word 2,$(subst ., ,$(VERSION)))
VERSION_PATCH := $(word 3,$(subst ., ,$(VERSION)))

CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

APP = mpk-mini-open
RESIDENT = mpk-mini-resident

# -MMD -MP records which headers each object used, so editing a header
# rebuilds what depends on it. Without this a header-only change quietly
# leaves stale objects in place and the resulting image is a mixture of
# two builds -- which is exactly as confusing to debug as it sounds.
CFLAGS = -mthumb -mcpu=cortex-m3 -Os -g3 -Wall -Wextra \
         -ffreestanding -nostdlib -Iinclude -MMD -MP \
         -DFIRMWARE_VERSION_MAJOR=$(VERSION_MAJOR) \
         -DFIRMWARE_VERSION_MINOR=$(VERSION_MINOR) \
         -DFIRMWARE_VERSION_PATCH=$(VERSION_PATCH)
LDFLAGS = -nostdlib -Wl,--gc-sections

# Sources shared by both images: the wire format, the flash primitives and
# the transfer state machine are identical in the application and in
# recovery, so an update can be pushed to either.
COMMON_SRCS = src/startup.c src/clock.c src/systick.c src/midi_ring.c \
              src/midi_uart.c src/sysex.c src/usb.c src/usb_descriptors.c \
              src/crc32.c src/sevenbit.c src/flash.c src/ota.c src/reset.c \
              src/app_image.c

APP_SRCS = $(COMMON_SRCS) src/main.c src/velocity.c src/matrix.c src/leds.c \
           src/program.c src/keys.c src/adc.c src/knobs.c src/pads.c \
           src/buttons.c src/transport.c src/arp.c

RESIDENT_SRCS = $(COMMON_SRCS) src/resident_main.c src/loader.c src/program_hold.c

APP_OBJS = $(patsubst src/%.c,build/app/%.o,$(APP_SRCS))
RESIDENT_OBJS = $(patsubst src/%.c,build/resident/%.o,$(RESIDENT_SRCS))

APP_CFLAGS = $(CFLAGS) -DVECTOR_BASE=0x08008000u
RESIDENT_CFLAGS = $(CFLAGS) -DVECTOR_BASE=0x08002000u -DRECOVERY_BUILD

all: build/$(APP).bin build/$(RESIDENT).bin
	@echo
	@echo "application (uploaded over the air, or flashed at 0x08008000):"
	@$(SIZE) build/$(APP).elf
	@echo
	@echo "resident loader + recovery (flashed once at 0x08002000):"
	@$(SIZE) build/$(RESIDENT).elf

build/app build/resident:
	mkdir -p $@

build/app/%.o: src/%.c | build/app
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/resident/%.o: src/%.c | build/resident
	$(CC) $(RESIDENT_CFLAGS) -c $< -o $@

build/$(APP).elf: $(APP_OBJS)
	$(CC) $(APP_CFLAGS) $(LDFLAGS) -Tlinker/app.ld \
	  -Wl,-Map=build/$(APP).map $(APP_OBJS) -o $@

build/$(RESIDENT).elf: $(RESIDENT_OBJS)
	$(CC) $(RESIDENT_CFLAGS) $(LDFLAGS) -Tlinker/resident.ld \
	  -Wl,-Map=build/$(RESIDENT).map $(RESIDENT_OBJS) -o $@

# The application image is raw: its length and CRC travel in the transfer
# itself and end up in the trailer the loader checks, so nothing has to be
# patched into the file.
build/$(APP).bin: build/$(APP).elf
	$(OBJCOPY) -O binary $< $@
	python3 tools/check_app_image.py $@
	python3 tools/make_trailer.py $@ build/$(APP)-trailer.bin

build/$(RESIDENT).bin: build/$(RESIDENT).elf tools/patch_checksum.py
	$(OBJCOPY) -O binary $< build/$(RESIDENT)-unsigned.bin
	python3 tools/patch_checksum.py build/$(RESIDENT)-unsigned.bin $@

-include $(APP_OBJS:.o=.d) $(RESIDENT_OBJS:.o=.d)

clean:
	rm -rf build

.PHONY: all clean
