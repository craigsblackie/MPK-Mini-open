TARGET = mpk-mini-open
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

CFLAGS = -mthumb -mcpu=cortex-m3 -Os -g3 -Wall -Wextra \
         -ffreestanding -nostdlib -Iinclude
LDFLAGS = -Tlinker/stm32f102.ld -nostdlib -Wl,--gc-sections -Wl,-Map=build/$(TARGET).map

SRCS = src/startup.c src/main.c src/velocity.c src/systick.c src/matrix.c src/leds.c src/midi_ring.c src/midi_uart.c src/program.c src/sysex.c src/keys.c src/adc.c src/knobs.c src/pads.c src/buttons.c src/transport.c src/arp.c src/usb.c src/usb_descriptors.c
OBJS = $(patsubst src/%.c,build/%.o,$(SRCS))

all: build/$(TARGET).elf build/$(TARGET).bin
	$(SIZE) build/$(TARGET).elf

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

build/$(TARGET).bin: build/$(TARGET).elf tools/patch_checksum.py
	$(OBJCOPY) -O binary $< build/$(TARGET)-unsigned.bin
	python3 tools/patch_checksum.py build/$(TARGET)-unsigned.bin $@

clean:
	rm -rf build

.PHONY: all clean
