#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

/*
 * CRC-32/ISO-HDLC -- the reflected, 0xedb88320 variant that zlib, gzip
 * and PNG use. Chosen over the STM32F1's hardware CRC unit because the
 * ESP32 side has to compute the identical value over the same bytes,
 * and the hardware unit's word-at-a-time, non-reflected MPEG-2 flavour
 * is fiddly to reproduce byte-for-byte on the other end. This file is
 * compiled into both firmwares, so there is one implementation and no
 * chance of the two drifting apart.
 *
 * A 16-entry nibble table keeps the cost to 64 bytes of flash; the
 * whole 26 KiB app slot checks in well under 20 ms at 48 MHz, which is
 * spent once per boot inside the loader.
 */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

/* One-shot over a single buffer. */
uint32_t crc32_compute(const void *data, size_t len);

/* Seed for an incremental run; finish with crc32_final(). */
#define CRC32_INIT 0xffffffffu
uint32_t crc32_final(uint32_t crc);

#endif /* CRC32_H */
