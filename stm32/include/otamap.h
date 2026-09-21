#ifndef OTAMAP_H
#define OTAMAP_H

#include <stdint.h>

/*
 * Flash layout for field-updatable firmware.
 *
 * The stock AKAI updater at 0x08000000 validates an additive checksum
 * over 0x08002000..0x080077fd and jumps through the vector table at
 * 0x08002000 (FIRMWARE_ANALYSIS.md, "bootloader/DFU entry at boot").
 * That region therefore cannot be rewritten in the field: any torn
 * write there fails the stock checksum and leaves the unit stranded in
 * an updater whose protocol needs a USB host.
 *
 * So the region the stock updater checksums holds something that never
 * changes -- a loader plus a minimal recovery application, installed
 * once over SWD -- and the real application lives above the program
 * store, outside anything the stock updater inspects. An over-the-air
 * update only ever erases the app slot and its trailer, so the worst a
 * power cut can do is leave no valid app, which the loader answers by
 * staying in recovery. The unit always boots something that can be
 * talked to.
 *
 *   0x08000000  stock AKAI updater                 8 KiB  untouched
 *   0x08002000  loader + recovery application     22 KiB  SWD once
 *   0x080077fe  stock updater's checksum halfword
 *   0x08007800  program store                      1 KiB  program.c
 *   0x08007c00  (reserved)                         1 KiB
 *   0x08008000  application slot                  26 KiB  update target
 *   0x0800e800  application trailer                1 KiB  update target
 *   0x0800ec00  (unused)                           5 KiB
 *
 * Everything here stays inside the 64 KiB the STM32F102R8T6 part
 * number guarantees, even though the verified dump reads 128 KiB.
 */

#define FLASH_PAGE_SIZE       0x400u
#define RESIDENT_BASE         0x08002000u
#define PROGRAM_STORE_BASE    0x08007800u

#define APP_SLOT_BASE         0x08008000u
#define APP_SLOT_SIZE         0x6800u
#define APP_SLOT_PAGES        (APP_SLOT_SIZE / FLASH_PAGE_SIZE)
#define APP_TRAILER_BASE      0x0800e800u

/* Smallest image the loader will consider: a vector table plus enough
 * code to be anything at all. Rejects a truncated upload outright. */
#define APP_MIN_SIZE          512u

#define RAM_BASE              0x20000000u
#define RAM_SIZE              (20u * 1024u)

/*
 * Trailer, written as the last act of an update. Its presence is what
 * makes an image bootable, so it is erased first and written last: at
 * every instant in between, the loader sees no valid app and falls
 * back to recovery rather than jumping into a half-written one.
 *
 *   0  magic  'M' 'P' 'K' 'A'
 *   4  length of the image in bytes, little endian
 *   8  CRC-32 (IEEE, reflected) over that many bytes from APP_SLOT_BASE
 *  12  ones' complement of the CRC -- guards against a trailer that was
 *      itself torn mid-write, without needing a second erase to detect
 */
#define APP_TRAILER_SIZE      16u
#define APP_TRAILER_MAGIC     0x414b504du /* 'M','P','K','A' little endian */

#endif /* OTAMAP_H */
