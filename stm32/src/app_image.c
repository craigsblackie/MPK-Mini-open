#include "app_image.h"
#include "otamap.h"
#include "flash.h"

struct trailer {
	uint32_t magic;
	uint32_t length;
	uint32_t crc;
	uint32_t inverse;
};

static void read_trailer(struct trailer *out)
{
	uint8_t bytes[APP_TRAILER_SIZE];
	flash_read(APP_TRAILER_BASE, bytes, sizeof bytes);

	out->magic = out->length = out->crc = out->inverse = 0;
	for (unsigned i = 0; i < 4; i++) {
		out->magic   |= (uint32_t)bytes[i]        << (8u * i);
		out->length  |= (uint32_t)bytes[4u + i]   << (8u * i);
		out->crc     |= (uint32_t)bytes[8u + i]   << (8u * i);
		out->inverse |= (uint32_t)bytes[12u + i]  << (8u * i);
	}
}

uint32_t app_image_length(void)
{
	uint32_t length = 0, crc = 0;
	(void)app_image_info(&length, &crc);
	return length;
}

bool app_image_info(uint32_t *length, uint32_t *crc)
{
	*length = 0;
	*crc = 0;
	if (!app_image_valid()) return false;

	struct trailer t;
	read_trailer(&t);
	*length = t.length;
	*crc = t.crc;
	return true;
}

bool app_image_valid(void)
{
	struct trailer t;
	read_trailer(&t);

	if (t.magic != APP_TRAILER_MAGIC) return false;
	/* A trailer that was itself torn mid-write: the magic landed but
	 * the rest did not. One erase invalidates it either way, but this
	 * catches the case without needing a second pass over flash. */
	if (t.inverse != ~t.crc) return false;
	if (t.length < APP_MIN_SIZE || t.length > APP_SLOT_SIZE) return false;
	if ((t.length & 1u) != 0u) return false;

	/* Check the image looks like a Cortex-M vector table before
	 * spending the CRC on it, and before trusting it enough to branch
	 * into it: a slot full of noise must not be jumped to even if some
	 * accident gave it a matching checksum. */
	uint32_t vectors[2];
	flash_read(APP_SLOT_BASE, vectors, sizeof vectors);
	if (vectors[0] < RAM_BASE || vectors[0] > RAM_BASE + RAM_SIZE) return false;
	if (vectors[1] < APP_SLOT_BASE || vectors[1] >= APP_SLOT_BASE + t.length) return false;
	if ((vectors[1] & 1u) == 0u) return false; /* Thumb bit */

	return flash_crc32(APP_SLOT_BASE, t.length) == t.crc;
}
