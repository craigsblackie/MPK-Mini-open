#ifndef VERSION_H
#define VERSION_H

/*
 * Reported over the 'f' query so the web editor can show what is
 * installed on each side of the link without the user having to
 * remember what they last flashed.
 *
 * The resident loader/recovery image carries whatever version it was
 * built from when it was installed over SWD; the application slot
 * carries the version that was last uploaded. They are allowed to
 * differ, and the editor shows both.
 */
/* Normally supplied by the build from version.txt, which the ESP32
 * project reads too. These are the fallback for a compile that does not
 * go through the Makefile, such as a host test. */
#ifndef FIRMWARE_VERSION_MAJOR
#define FIRMWARE_VERSION_MAJOR 0
#endif
#ifndef FIRMWARE_VERSION_MINOR
#define FIRMWARE_VERSION_MINOR 0
#endif
#ifndef FIRMWARE_VERSION_PATCH
#define FIRMWARE_VERSION_PATCH 0
#endif

#endif /* VERSION_H */
