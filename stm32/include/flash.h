#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Erase and program helpers for the on-chip flash.
 *
 * program.c has its own open-coded copy of this sequence for the
 * program store. That duplication is deliberate: program.c is compiled
 * unmodified into the ESP32 project's host tests, and is the one piece
 * of flash handling already validated on the physical board, so it is
 * left exactly as it was rather than refactored underneath a new API.
 *
 * On STM32F1 the flash interface stalls the bus while a page erase or a
 * halfword program is in flight, so code fetched from flash simply
 * pauses until the operation finishes (RM0008 3.3.3). There is no need
 * to relocate any of this into SRAM -- and nothing here ever touches
 * the page it is executing from, since the update target is the app
 * slot and the loader that calls it lives in the resident region.
 */

void flash_unlock(void);
void flash_lock(void);

/* Erase the 1 KiB page containing `address`. False on a flash error. */
bool flash_erase_page(uint32_t address);

/* Program `len` bytes (which must be even, to an even address). False on
 * a flash error or a readback mismatch. */
bool flash_program(uint32_t address, const uint8_t *data, size_t len);

/* True if every byte in the range reads as erased. */
bool flash_is_erased(uint32_t address, size_t len);

/* Copy out of flash. ota.c reads the app slot only through this and
 * flash_crc32(), so the whole update state machine can be compiled and
 * exercised on a host against a plain array standing in for flash. */
void flash_read(uint32_t address, void *dst, size_t len);

/* CRC-32 (see crc32.h) over a flash range, without copying it first. */
uint32_t flash_crc32(uint32_t address, size_t len);

#endif /* FLASH_H */
