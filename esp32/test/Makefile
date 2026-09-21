# Host-side tests for the bridge. Both suites compile the real firmware
# sources against the stub ESP-IDF/NimBLE headers in stub/ -- no hardware,
# no ESP-IDF install needed.
#
#   test        BLE-MIDI translation: encode a stream, decode it back,
#               assert the recovered bytes match what went in.
#   editor_test the editor's SysEx translation, driven against a fake
#               keyboard that answers 'c'/'a'/'v'/'d' like the real one.
#               Also checks editor.c's copy of the firmware's wire-reorder
#               table against stm32/src/program.c, byte for byte.
#   firmware_update_test
#               the firmware transfer end to end, with the keyboard's real
#               receiving code on the other side of the link.

CFLAGS = -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined \
         -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
         -Istub -I../src
FIRMWARE_PROGRAM_C = ../../stm32/src/program.c

STUBS = $(wildcard stub/*.h stub/*/*.h stub/*/*/*.h)

test: test.c ../src/main.c $(STUBS)
	$(CC) $(CFLAGS) test.c -o $@

editor_test: editor_test.c ../src/editor.c ../src/sysex_bridge.h $(STUBS)
	$(CC) $(CFLAGS) -DEDITOR_HOST_TEST \
	  -DFIRMWARE_PROGRAM_C='"$(FIRMWARE_PROGRAM_C)"' editor_test.c -o $@

# Both ends of the firmware transfer, for real: the bridge's sender from
# ../main and the keyboard's receiver from ../../stm32/src, with an
# array standing in for the STM32's flash. The messages crossing the
# harness are the bytes that would cross the UART.
FIRMWARE_SRC = ../../stm32/src
firmware_update_test: firmware_update_test.c ../src/firmware_update.c \
                      $(FIRMWARE_SRC)/ota.c $(FIRMWARE_SRC)/app_image.c \
                      $(FIRMWARE_SRC)/crc32.c $(FIRMWARE_SRC)/sevenbit.c $(STUBS)
	$(CC) $(CFLAGS) -I../../stm32/include -DRECOVERY_BUILD firmware_update_test.c \
	  $(FIRMWARE_SRC)/ota.c $(FIRMWARE_SRC)/app_image.c \
	  $(FIRMWARE_SRC)/crc32.c $(FIRMWARE_SRC)/sevenbit.c -o $@

check: test editor_test firmware_update_test
	./test
	@echo
	./editor_test
	@echo
	./firmware_update_test

clean:
	rm -f test editor_test firmware_update_test

.PHONY: check clean
