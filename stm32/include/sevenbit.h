#ifndef SEVENBIT_H
#define SEVENBIT_H

#include <stdint.h>
#include <stddef.h>

/*
 * The 7-in-8 packing every SysEx bulk-dump format uses, because no byte
 * between F0 and F7 may have its top bit set.
 *
 * Each group of seven source bytes becomes eight wire bytes: a leading
 * byte carrying their seven high bits (source byte 0's MSB in bit 0),
 * then the seven low-7-bit remainders. A trailing partial group is
 * emitted the same way, shortened -- the leading byte still carries one
 * bit per byte actually present.
 *
 * Compiled into both the STM32 firmware and the ESP32 bridge so the
 * encoder and decoder can never disagree about the layout.
 */

/* Wire bytes produced by encoding `len` source bytes. */
size_t sevenbit_encoded_size(size_t len);

/* Source bytes recovered from `len` wire bytes, or 0 if `len` cannot be
 * a valid encoding (a group of exactly one byte carries no payload). */
size_t sevenbit_decoded_size(size_t len);

/* Encode `len` bytes into `out`, which must hold sevenbit_encoded_size(len).
 * Returns the number of wire bytes written. */
size_t sevenbit_encode(const uint8_t *in, size_t len, uint8_t *out);

/* Decode `len` wire bytes into `out`, which must hold
 * sevenbit_decoded_size(len). Returns the number of source bytes
 * written, or 0 if the input is malformed or any byte has bit 7 set. */
size_t sevenbit_decode(const uint8_t *in, size_t len, uint8_t *out);

#endif /* SEVENBIT_H */
