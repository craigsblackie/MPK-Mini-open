#include "crc32.h"

/* Nibble table: the low four bits of the reflected polynomial's
 * byte table. Two lookups per byte instead of one, for 64 bytes of
 * table instead of 1024. */
static const uint32_t nibble_table[16] = {
	0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
	0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
	0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
	0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
};

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *bytes = (const uint8_t *)data;
	while (len-- != 0) {
		crc ^= *bytes++;
		crc = (crc >> 4) ^ nibble_table[crc & 0x0fu];
		crc = (crc >> 4) ^ nibble_table[crc & 0x0fu];
	}
	return crc;
}

uint32_t crc32_final(uint32_t crc)
{
	return crc ^ 0xffffffffu;
}

uint32_t crc32_compute(const void *data, size_t len)
{
	return crc32_final(crc32_update(CRC32_INIT, data, len));
}
