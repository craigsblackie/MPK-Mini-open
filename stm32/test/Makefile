# Host-side tests for the parts of the firmware that are pure logic.
#
# velocity.c has no hardware dependencies, so it compiles and runs here
# directly. The flash page layout constants are mirrored from program.c
# and asserted against the real page size, so a settings block that grows
# past the page is caught at test time rather than by corrupting a device.

CFLAGS = -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -I../include

velocity_test: velocity_test.c ../src/velocity.c ../include/velocity.h
	$(CC) $(CFLAGS) velocity_test.c ../src/velocity.c -lm -o $@

transport_test: transport_test.c ../src/transport.c ../include/transport.h
	$(CC) $(CFLAGS) transport_test.c ../src/transport.c -o $@

# ota.c and app_image.c reach flash and the clock only through flash.h,
# systick.h and one reset hook, all of which ota_test.c supplies -- so the
# update path that decides whether a unit boots is exercised here rather
# than only on hardware.
ota_test: ota_test.c ../src/ota.c ../src/app_image.c ../src/crc32.c ../src/sevenbit.c \
          ../include/ota.h ../include/otamap.h ../include/otaproto.h
	$(CC) $(CFLAGS) ota_test.c ../src/ota.c ../src/app_image.c ../src/crc32.c \
	  ../src/sevenbit.c -o $@

program_hold_test: program_hold_test.c ../src/program_hold.c ../include/program_hold.h
	$(CC) $(CFLAGS) program_hold_test.c ../src/program_hold.c -o $@

check: velocity_test transport_test program_hold_test ota_test
	./velocity_test
	./transport_test
	@echo
	./program_hold_test
	@echo
	./ota_test

clean:
	rm -f velocity_test transport_test program_hold_test ota_test

.PHONY: check clean
