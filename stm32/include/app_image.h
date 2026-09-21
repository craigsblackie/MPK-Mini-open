#ifndef APP_IMAGE_H
#define APP_IMAGE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Is there a bootable application in the slot?
 *
 * Split out of loader.c so it can be compiled and exercised on a host
 * against an array standing in for flash (test/ota_test.c). This is the
 * single gate between "the unit works" and "the unit is a brick", and
 * it is reached through flash.h for exactly that reason -- every way an
 * update can be interrupted or corrupted is cheap to reproduce here and
 * expensive to reproduce on the bench.
 */
bool app_image_valid(void);

/* Length recorded in the trailer. Only meaningful when the image is
 * valid; zero otherwise. */
uint32_t app_image_length(void);

/*
 * The installed image's length and CRC-32, for reporting to whoever
 * might want to download a copy of it. Returns false and leaves both at
 * zero when no valid application is installed -- the normal state in
 * recovery, and the honest answer to "what have you got?".
 */
bool app_image_info(uint32_t *length, uint32_t *crc);

#endif /* APP_IMAGE_H */
