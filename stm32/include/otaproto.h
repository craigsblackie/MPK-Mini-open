#ifndef OTAPROTO_H
#define OTAPROTO_H

/*
 * The 'f' SysEx command: firmware transfer.
 *
 * Framed exactly like every other editor command --
 *
 *   F0 47 <id> 7C 'f' <len_hi> <len_lo> <sub> <payload...> F7
 *
 * -- with byte 7 reused as a sub-command, the way 'v' reuses it for the
 * velocity settings (sysex.c). The length field counts byte 7 plus the
 * payload and is a 14-bit value, high 7 bits first, because a data
 * chunk overruns a single 7-bit byte.
 *
 * 'f' is safe to claim: stock uses only 'a', 'b', 'c', 'd', 'j' and '`'
 * (FIRMWARE_ANALYSIS.md's command table), and this project has already
 * taken 'v'. A stock editor will never emit it.
 *
 * Payload bytes that carry arbitrary data are 7-in-8 packed (sevenbit.h);
 * scalar fields are sent as fixed runs of 7-bit digits, most significant
 * first, which is what the stock protocol does for its own 14-bit
 * lengths and tempos.
 *
 * Both firmwares include this header, so there is one definition of the
 * wire format rather than two that have to be kept in step.
 */

#define OTA_CMD              'f'

#define OTA_SUB_QUERY        0x00
#define OTA_SUB_BEGIN        0x01
#define OTA_SUB_DATA         0x02
#define OTA_SUB_COMMIT       0x03
#define OTA_SUB_ABORT        0x04
#define OTA_SUB_READ         0x05

/*
 * Bumped only if the framing changes incompatibly. The ESP refuses to
 * start a transfer it does not recognise rather than guessing.
 *
 * Deliberately still 1 now that OTA_SUB_READ and the two extra query
 * fields exist, because both additions are backward compatible in the
 * direction that matters. The resident recovery image cannot be updated
 * in the field -- that is the whole point of it -- so a version bump
 * would strand any unit whose recovery image predates the change,
 * refusing to talk to the one thing that can rescue it. Instead:
 * readers accept a query reply of at least OTA_QUERY_EXTRA_V1 bytes and
 * treat the rest as absent if it is not there, and an image that does
 * not know OTA_SUB_READ answers it with OTA_STATUS_STATE, which reads
 * as "this one cannot do downloads" rather than as a broken link.
 */
#define OTA_PROTOCOL_VERSION 1

/* Which image is answering: the field-updatable application, or the
 * resident recovery application that runs when no valid app is installed. */
#define OTA_MODE_APP         0
#define OTA_MODE_RECOVERY    1

#define OTA_STATUS_OK        0
#define OTA_STATUS_STATE     1  /* command out of sequence */
#define OTA_STATUS_RANGE     2  /* length or offset outside the slot */
#define OTA_STATUS_FLASH     3  /* erase or program reported an error */
#define OTA_STATUS_CRC       4  /* image did not match the stated CRC */
#define OTA_STATUS_ENCODING  5  /* payload was not valid 7-in-8 packing */
#define OTA_STATUS_SEQUENCE  6  /* chunk offset was not the expected one */
/*
 * Not an error: the application cannot rewrite the slot it is executing
 * from, so it has invalidated itself and is restarting into recovery,
 * which runs from the resident region and can. The sender waits for it
 * to come back and starts the transfer again.
 */
#define OTA_STATUS_REBOOTING 7

/*
 * Largest raw chunk carried by one OTA_SUB_DATA message: 112 bytes,
 * exactly sixteen 7-in-8 groups, so it packs to 128 wire bytes with no
 * partial group. With the 4-byte offset that makes a 132-byte payload
 * and a 141-byte message, inside sysex.c's buffer with room to spare.
 *
 * It must stay even so that every chunk lands on a halfword boundary,
 * which is the only granularity STM32F1 flash can be programmed at.
 */
#define OTA_CHUNK_BYTES      112u
#define OTA_CHUNK_ENCODED    128u

/* Offsets and lengths travel as 4 seven-bit digits (28 bits), CRCs as
 * 5 (35 bits). Both are more than the 26 KiB slot and a 32-bit CRC need. */
#define OTA_DIGITS_OFFSET    4u
#define OTA_DIGITS_CRC       5u
/* The slot is 26 KiB, so a length fits in three seven-bit digits. */
#define OTA_DIGITS_LENGTH    3u
/* A READ asks for at most OTA_CHUNK_BYTES, which fits in two. */
#define OTA_DIGITS_COUNT     2u

/*
 * Bytes a QUERY reply carries after its status byte: protocol version,
 * which image is running, three version numbers, the slot size as three
 * digits and the largest chunk it will accept as two. That much has
 * always been there.
 *
 * Followed now by the installed application's length as three digits
 * and its CRC-32 as five, both taken from the trailer, so the other end
 * knows exactly how many bytes to ask for when downloading it and what
 * the result should check out to. Both are zero when no valid
 * application is installed, which is the normal state in recovery.
 */
#define OTA_QUERY_EXTRA_V1   10u
#define OTA_QUERY_EXTRA      18u

/* Every message starts F0 47 <id> 7C 'f' <len_hi> <len_lo> <sub>. */
#define OTA_HEADER_LEN       8u

#endif /* OTAPROTO_H */
