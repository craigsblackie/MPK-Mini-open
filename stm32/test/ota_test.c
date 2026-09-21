/*
 * Host-side tests for the firmware-transfer path.
 *
 * ota.c and app_image.c reach flash only through flash.h, so both
 * compile here against the array below standing in for the application
 * slot. That is the whole point of routing them that way: the failure
 * being designed against is a unit that will not boot, and every way an
 * update can be interrupted is cheap to reproduce here and slow and
 * risky to reproduce on the bench.
 *
 * The fake flash enforces the rules real NOR flash does -- erased bytes
 * are 0xff, programming can only clear bits, and a write to an unerased
 * halfword fails -- so a test that passes here is not passing because
 * the stand-in was more forgiving than the hardware.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "crc32.h"
#include "flash.h"
#include "otamap.h"
#include "otaproto.h"
#include "sevenbit.h"
#include "ota.h"
#include "app_image.h"

/* ---------- fake flash ---------- */

#define FAKE_BASE APP_SLOT_BASE
#define FAKE_SIZE (APP_TRAILER_BASE + FLASH_PAGE_SIZE - APP_SLOT_BASE)

static uint8_t fake[FAKE_SIZE];
static int flash_fails_after = -1;   /* -1: never */
static unsigned erase_count;
static unsigned program_count;

static uint8_t *at(uint32_t address)
{
	assert(address >= FAKE_BASE && address < FAKE_BASE + FAKE_SIZE);
	return &fake[address - FAKE_BASE];
}

static bool failing(void)
{
	if (flash_fails_after < 0) return false;
	if (flash_fails_after == 0) return true;
	flash_fails_after--;
	return false;
}

void flash_unlock(void) {}
void flash_lock(void) {}

bool flash_erase_page(uint32_t address)
{
	assert((address % FLASH_PAGE_SIZE) == 0);
	if (failing()) return false;
	erase_count++;
	memset(at(address), 0xff, FLASH_PAGE_SIZE);
	return true;
}

bool flash_program(uint32_t address, const uint8_t *data, size_t len)
{
	assert((address & 1u) == 0 && (len & 1u) == 0);
	if (failing()) return false;
	program_count++;
	uint8_t *dst = at(address);
	for (size_t i = 0; i < len; i++) {
		/* NOR flash can only clear bits; programming over anything
		 * not erased is a hardware error, not a silent overwrite. */
		if (dst[i] != 0xffu && dst[i] != data[i]) return false;
		dst[i] &= data[i];
		if (dst[i] != data[i]) return false;
	}
	return true;
}

bool flash_is_erased(uint32_t address, size_t len)
{
	const uint8_t *p = at(address);
	for (size_t i = 0; i < len; i++) if (p[i] != 0xffu) return false;
	return true;
}

void flash_read(uint32_t address, void *dst, size_t len)
{
	memcpy(dst, at(address), len);
}

uint32_t flash_crc32(uint32_t address, size_t len)
{
	return crc32_compute(at(address), len);
}

/* ---------- other platform hooks ---------- */

static uint32_t fake_millis;
uint32_t systick_millis(void) { return fake_millis; }

static int reset_count;
void ota_reset_system(void) { reset_count++; }

/* ota.c builds its own replies and hands them here; on the device this
 * is sysex.c packing them into USB-MIDI events. */
static uint8_t reply[64];
static size_t reply_len;
void ota_emit(const uint8_t *message, size_t len)
{
	assert(len <= sizeof reply);
	memcpy(reply, message, len);
	reply_len = len;
}

/* ---------- sender side, mirroring what the ESP32 does ---------- */

static uint32_t read_digits(const uint8_t *digits, unsigned count)
{
	uint32_t value = 0;
	for (unsigned i = 0; i < count; i++)
		value = (value << 7) | (uint32_t)(digits[i] & 0x7fu);
	return value;
}

static void write_digits(uint8_t *out, unsigned count, uint32_t value)
{
	for (unsigned i = 0; i < count; i++)
		out[count - 1u - i] = (uint8_t)((value >> (7u * i)) & 0x7fu);
}

static uint8_t send_begin(uint32_t length)
{
	uint8_t payload[OTA_DIGITS_OFFSET];
	write_digits(payload, OTA_DIGITS_OFFSET, length);
	return ota_begin(payload, sizeof payload);
}

static uint8_t send_chunk(uint32_t offset, const uint8_t *data, size_t len,
                          uint32_t *received)
{
	uint8_t payload[OTA_DIGITS_OFFSET + OTA_CHUNK_ENCODED];
	write_digits(payload, OTA_DIGITS_OFFSET, offset);
	size_t encoded = sevenbit_encode(data, len, &payload[OTA_DIGITS_OFFSET]);
	return ota_data(payload, OTA_DIGITS_OFFSET + encoded, received);
}

static uint8_t send_commit(uint32_t crc)
{
	uint8_t payload[OTA_DIGITS_CRC];
	write_digits(payload, OTA_DIGITS_CRC, crc);
	return ota_commit(payload, sizeof payload);
}

/* Upload `len` bytes, stopping early after `stop_after` chunks if that
 * is non-negative -- which is how a power cut mid-transfer is modelled. */
static uint8_t upload(const uint8_t *image, uint32_t len, int stop_after)
{
	uint8_t status = send_begin(len);
	if (status != OTA_STATUS_OK) return status;

	int chunks = 0;
	for (uint32_t offset = 0; offset < len; offset += OTA_CHUNK_BYTES) {
		if (stop_after >= 0 && chunks == stop_after) return OTA_STATUS_OK;
		uint32_t remaining = len - offset;
		size_t take = remaining < OTA_CHUNK_BYTES ? remaining : OTA_CHUNK_BYTES;
		uint32_t received = 0;
		status = send_chunk(offset, &image[offset], take, &received);
		if (status != OTA_STATUS_OK) return status;
		assert(received == offset + take);
		chunks++;
		fake_millis += 50;
	}
	if (stop_after >= 0) return OTA_STATUS_OK;
	return send_commit(crc32_compute(image, len));
}

/* ---------- a plausible application image ---------- */

static uint8_t image[14084];
static uint32_t image_len = sizeof image;

static void build_image(void)
{
	uint32_t stack = RAM_BASE + RAM_SIZE;
	uint32_t entry = APP_SLOT_BASE + 0x97u;
	memcpy(&image[0], &stack, 4);
	memcpy(&image[4], &entry, 4);
	for (uint32_t i = 8; i < image_len; i++)
		image[i] = (uint8_t)(i * 31u + (i >> 5));
}

static void reset_world(void)
{
	memset(fake, 0xff, sizeof fake);
	flash_fails_after = -1;
	erase_count = program_count = 0;
	reset_count = 0;
	fake_millis = 1000;
	ota_init();
}

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		exit(1); \
	} \
} while (0)

/* ---------- tests ---------- */

static void test_sevenbit_roundtrip(void)
{
	uint8_t source[300], encoded[512], decoded[300];
	for (size_t len = 0; len <= sizeof source; len++) {
		for (size_t i = 0; i < len; i++) source[i] = (uint8_t)(i * 7u + (i >> 3));
		size_t n = sevenbit_encode(source, len, encoded);
		CHECK(n == sevenbit_encoded_size(len));
		for (size_t i = 0; i < n; i++) CHECK((encoded[i] & 0x80u) == 0);
		CHECK(sevenbit_decoded_size(n) == len);
		CHECK(sevenbit_decode(encoded, n, decoded) == len);
		CHECK(memcmp(source, decoded, len) == 0);
	}

	/* A full chunk must pack to exactly the size the protocol reserves,
	 * with no partial group -- that is why 112 was chosen. */
	CHECK(sevenbit_encoded_size(OTA_CHUNK_BYTES) == OTA_CHUNK_ENCODED);

	/* Malformed input is rejected rather than half-decoded. */
	uint8_t lone_msb[1] = {0};
	CHECK(sevenbit_decode(lone_msb, 1, decoded) == 0);
	uint8_t high_bit[2] = {0x00, 0x80};
	CHECK(sevenbit_decode(high_bit, 2, decoded) == 0);
	printf("  7-in-8 codec round-trips every length to 300 bytes\n");
}

static void test_crc32_known_vectors(void)
{
	/* The published CRC-32/ISO-HDLC check values. The ESP32 computes
	 * the same function over the same bytes, so this is what pins the
	 * two ends together. */
	CHECK(crc32_compute("", 0) == 0x00000000u);
	CHECK(crc32_compute("123456789", 9) == 0xcbf43926u);
	CHECK(crc32_compute("a", 1) == 0xe8b7be43u);
	printf("  CRC-32 matches the published check values\n");
}

static void test_complete_upload_boots(void)
{
	reset_world();
	CHECK(!app_image_valid());
	CHECK(upload(image, image_len, -1) == OTA_STATUS_OK);
	CHECK(app_image_valid());
	CHECK(app_image_length() == image_len);
	CHECK(memcmp(at(APP_SLOT_BASE), image, image_len) == 0);

	/* The reboot waits for the reply to drain, then happens. */
	CHECK(ota_reboot_pending());
	ota_process();
	CHECK(reset_count == 0);
	fake_millis += 1000;
	ota_process();
	CHECK(reset_count == 1);
	printf("  a complete upload lands byte-for-byte and boots\n");
}

static void test_interrupted_upload_falls_back(void)
{
	/* The promise the layout is built on: at no instant between the
	 * start of an update and its commit does the loader consider the
	 * slot bootable. Checked at every chunk boundary, not just one. */
	int total_chunks = (int)((image_len + OTA_CHUNK_BYTES - 1) / OTA_CHUNK_BYTES);
	for (int stop = 0; stop <= total_chunks; stop++) {
		reset_world();
		CHECK(upload(image, image_len, -1) == OTA_STATUS_OK);
		CHECK(app_image_valid());

		/* Now a second update is interrupted `stop` chunks in. */
		CHECK(upload(image, image_len, stop) == OTA_STATUS_OK);
		CHECK(!app_image_valid());
	}
	printf("  an upload interrupted at any of %d chunk boundaries leaves recovery\n",
	       total_chunks + 1);
}

static void test_corrupt_image_is_rejected(void)
{
	reset_world();
	uint8_t bad[sizeof image];
	memcpy(bad, image, image_len);
	CHECK(send_begin(image_len) == OTA_STATUS_OK);
	for (uint32_t offset = 0; offset < image_len; offset += OTA_CHUNK_BYTES) {
		uint32_t remaining = image_len - offset;
		size_t take = remaining < OTA_CHUNK_BYTES ? remaining : OTA_CHUNK_BYTES;
		uint32_t received;
		CHECK(send_chunk(offset, &bad[offset], take, &received) == OTA_STATUS_OK);
	}
	/* Commit with the CRC of a different image: the bytes are all
	 * there, but they are not the bytes that were promised. */
	CHECK(send_commit(crc32_compute(image, image_len) ^ 1u) == OTA_STATUS_CRC);
	CHECK(!app_image_valid());
	CHECK(!ota_reboot_pending());
	printf("  a CRC mismatch refuses to commit and leaves recovery\n");
}

static void test_protocol_guards(void)
{
	reset_world();

	/* Data before begin. */
	uint32_t received;
	CHECK(send_chunk(0, image, 112, &received) == OTA_STATUS_STATE);

	/* Lengths the slot cannot hold, or that are not whole halfwords. */
	CHECK(send_begin(APP_SLOT_SIZE + 2u) == OTA_STATUS_RANGE);
	CHECK(send_begin(APP_MIN_SIZE - 2u) == OTA_STATUS_RANGE);
	CHECK(send_begin(1025) == OTA_STATUS_RANGE);
	CHECK(send_begin(APP_SLOT_SIZE) == OTA_STATUS_OK); /* exactly full fits */

	reset_world();
	CHECK(send_begin(image_len) == OTA_STATUS_OK);

	/* Out-of-order chunks are refused, and the reply says where the
	 * receiver actually is so the sender can restart knowingly. */
	CHECK(send_chunk(OTA_CHUNK_BYTES, image, 112, &received) == OTA_STATUS_SEQUENCE);
	CHECK(received == 0);
	CHECK(send_chunk(0, image, 112, &received) == OTA_STATUS_OK);
	CHECK(received == OTA_CHUNK_BYTES);
	CHECK(send_chunk(0, image, 112, &received) == OTA_STATUS_SEQUENCE);
	CHECK(received == OTA_CHUNK_BYTES);

	/* Overrunning the declared length. 1024 bytes holds nine whole
	 * 112-byte chunks; the tenth must be refused rather than spilling
	 * past what the sender said it was sending. */
	CHECK(send_begin(1024) == OTA_STATUS_OK);
	for (int i = 0; i < 10; i++) {
		uint8_t status = send_chunk((uint32_t)i * OTA_CHUNK_BYTES,
		                            image, OTA_CHUNK_BYTES, &received);
		CHECK(status == (i < 9 ? OTA_STATUS_OK : OTA_STATUS_RANGE));
	}

	/* Committing a short image. */
	reset_world();
	CHECK(send_begin(image_len) == OTA_STATUS_OK);
	CHECK(send_chunk(0, image, 112, &received) == OTA_STATUS_OK);
	CHECK(send_commit(0) == OTA_STATUS_STATE);

	/* Malformed 7-in-8 payloads. A byte with bit 7 set cannot reach
	 * here through a real SysEx stream -- the parser would have ended
	 * the message -- but the decoder refuses it rather than folding it
	 * into the image. */
	reset_world();
	CHECK(send_begin(image_len) == OTA_STATUS_OK);
	uint8_t high_bit[OTA_DIGITS_OFFSET + 3] = {0, 0, 0, 0, 0x00, 0x80, 0x7f};
	CHECK(ota_data(high_bit, sizeof high_bit, &received) == OTA_STATUS_ENCODING);
	uint8_t lone_msb[OTA_DIGITS_OFFSET + 1] = {0, 0, 0, 0, 0x00};
	CHECK(ota_data(lone_msb, sizeof lone_msb, &received) == OTA_STATUS_ENCODING);

	/* A chunk larger than the receiver's buffer. */
	uint8_t oversized[OTA_DIGITS_OFFSET + OTA_CHUNK_ENCODED + 8] = {0};
	CHECK(ota_data(oversized, sizeof oversized, &received) == OTA_STATUS_RANGE);

	/* An odd-length chunk cannot be programmed into halfword flash. */
	uint8_t odd[OTA_DIGITS_OFFSET + 4] = {0, 0, 0, 0, 0x00, 0x11, 0x22, 0x33};
	CHECK(ota_data(odd, sizeof odd, &received) == OTA_STATUS_RANGE);
	CHECK(received == 0);
	printf("  out-of-sequence, oversized and malformed messages are refused\n");
}

static void test_flash_errors_surface(void)
{
	/* A flash error anywhere must be reported, never silently ignored,
	 * and must not leave a trailer behind. */
	reset_world();
	flash_fails_after = 0;
	CHECK(send_begin(image_len) == OTA_STATUS_FLASH);

	reset_world();
	CHECK(send_begin(image_len) == OTA_STATUS_OK);
	flash_fails_after = 0;
	uint32_t received;
	CHECK(send_chunk(0, image, 112, &received) == OTA_STATUS_FLASH);
	CHECK(!app_image_valid());
	printf("  erase and program failures surface as flash errors\n");
}

static void test_stalled_transfer_is_abandoned(void)
{
	reset_world();
	CHECK(send_begin(image_len) == OTA_STATUS_OK);
	uint32_t received;
	CHECK(send_chunk(0, image, 112, &received) == OTA_STATUS_OK);

	fake_millis += 60000;
	ota_process();

	/* The slot is not locked against the next attempt. */
	CHECK(send_chunk(OTA_CHUNK_BYTES, image, 112, &received) == OTA_STATUS_STATE);
	CHECK(upload(image, image_len, -1) == OTA_STATUS_OK);
	CHECK(app_image_valid());
	printf("  an abandoned transfer does not lock the slot\n");
}

static void test_boot_gate_rejects_nonsense(void)
{
	/* Everything the loader is the last line of defence against. */
	reset_world();
	CHECK(upload(image, image_len, -1) == OTA_STATUS_OK);
	CHECK(app_image_valid());

	uint8_t good_trailer[APP_TRAILER_SIZE];
	memcpy(good_trailer, at(APP_TRAILER_BASE), sizeof good_trailer);

	struct { const char *what; unsigned offset; uint8_t value; } corruptions[] = {
		{"a wrong magic",            0,  'X'},
		{"a torn trailer",           12, 0x00},
		{"a length past the slot",   7,  0xff},
	};
	for (unsigned i = 0; i < sizeof corruptions / sizeof corruptions[0]; i++) {
		memcpy(at(APP_TRAILER_BASE), good_trailer, sizeof good_trailer);
		at(APP_TRAILER_BASE)[corruptions[i].offset] = corruptions[i].value;
		CHECK(!app_image_valid());
	}

	/* A single flipped bit anywhere in the image. */
	memcpy(at(APP_TRAILER_BASE), good_trailer, sizeof good_trailer);
	CHECK(app_image_valid());
	at(APP_SLOT_BASE)[image_len / 2] ^= 0x01u;
	CHECK(!app_image_valid());
	at(APP_SLOT_BASE)[image_len / 2] ^= 0x01u;

	/* A vector table that would fault on the first instruction. */
	uint32_t original_entry;
	memcpy(&original_entry, at(APP_SLOT_BASE + 4), 4);
	uint32_t even_entry = original_entry & ~1u;          /* no Thumb bit */
	uint32_t outside = APP_SLOT_BASE + APP_SLOT_SIZE;    /* past the image */
	uint32_t vectors[] = {even_entry, outside, 0x08000000u};
	for (unsigned i = 0; i < 3; i++) {
		memcpy(at(APP_SLOT_BASE + 4), &vectors[i], 4);
		CHECK(!app_image_valid());
	}
	memcpy(at(APP_SLOT_BASE + 4), &original_entry, 4);

	uint32_t bad_stack = 0x08001234u;                    /* not SRAM */
	uint32_t good_stack;
	memcpy(&good_stack, at(APP_SLOT_BASE), 4);
	memcpy(at(APP_SLOT_BASE), &bad_stack, 4);
	CHECK(!app_image_valid());
	memcpy(at(APP_SLOT_BASE), &good_stack, 4);

	/* An entirely erased slot is the state a fresh install is in. */
	memset(fake, 0xff, sizeof fake);
	CHECK(!app_image_valid());
	printf("  the boot gate rejects wrong magic, torn trailers, bit flips and bad vectors\n");
}

/* The framing itself, driven the way sysex.c drives it. The ESP32's own
 * tests run its sender against this same entry point, so a disagreement
 * about the wire format shows up on one side or the other. */
static void test_message_layer(void)
{
	reset_world();
	reply_len = 0;

	uint8_t query[] = {0xf0, 0x47, 0x42, 0x7c, OTA_CMD, 0x00, 0x01,
	                   OTA_SUB_QUERY, 0xf7};
	ota_handle_message(query, sizeof query, 0x42);

	CHECK(reply_len == 8u + 1u + OTA_QUERY_EXTRA + 1u);
	CHECK(reply[0] == 0xf0 && reply[1] == 0x47 && reply[2] == 0x42);
	CHECK(reply[3] == 0x7c && reply[4] == OTA_CMD);
	CHECK(reply[7] == OTA_SUB_QUERY);
	CHECK(reply[8] == OTA_STATUS_OK);
	CHECK(reply[reply_len - 1] == 0xf7);
	for (size_t i = 0; i < reply_len; i++)
		CHECK(i == 0 || i == reply_len - 1 || (reply[i] & 0x80u) == 0);

	/* The declared length counts byte 7 plus the payload, high seven
	 * bits first -- the same convention the stock commands use. */
	uint16_t declared = (uint16_t)(((uint16_t)reply[5] << 7) | reply[6]);
	CHECK(declared == 1u + 1u + OTA_QUERY_EXTRA);

	CHECK(reply[9] == OTA_PROTOCOL_VERSION);
	CHECK(reply[10] == OTA_MODE_APP);

	/* An unknown sub-command is answered, not ignored: a sender must
	 * never be left waiting on a reply that is not coming. */
	reply_len = 0;
	uint8_t unknown[] = {0xf0, 0x47, 0x00, 0x7c, OTA_CMD, 0x00, 0x01, 0x7f, 0xf7};
	ota_handle_message(unknown, sizeof unknown, 0x00);
	CHECK(reply_len != 0);
	CHECK(reply[7] == 0x7f);
	CHECK(reply[8] != OTA_STATUS_OK);
	printf("  the 'f' message layer frames replies the protocol describes\n");
}

/* Reading the installed image back out -- the download path. */
static uint8_t send_read(uint32_t offset, uint32_t count, uint8_t *out,
                         uint32_t *got_offset, uint32_t *got_count)
{
	uint8_t payload[OTA_DIGITS_OFFSET + OTA_DIGITS_COUNT];
	write_digits(payload, OTA_DIGITS_OFFSET, offset);
	write_digits(&payload[OTA_DIGITS_OFFSET], OTA_DIGITS_COUNT, count);
	return ota_read(payload, sizeof payload, out, got_offset, got_count);
}

static void test_download_round_trip(void)
{
	reset_world();

	/* Nothing installed: there is nothing to hand out, and saying so
	 * is better than handing out an erased slot. */
	uint8_t chunk[OTA_CHUNK_BYTES];
	uint32_t off = 0, count = 0;
	CHECK(send_read(0, OTA_CHUNK_BYTES, chunk, &off, &count) == OTA_STATUS_STATE);

	CHECK(upload(image, image_len, -1) == OTA_STATUS_OK);
	CHECK(app_image_valid());

	/* A commit leaves the reboot pending; on hardware the unit resets
	 * and comes back with the transfer state clear. Model that, because
	 * a download only makes sense once it has. */
	fake_millis += 1000;
	ota_process();
	CHECK(reset_count == 1);
	ota_init();

	/* Read the whole thing back and compare with what went in. */
	static uint8_t recovered[sizeof image];
	uint32_t at = 0;
	while (at < image_len) {
		CHECK(send_read(at, OTA_CHUNK_BYTES, chunk, &off, &count) == OTA_STATUS_OK);
		CHECK(off == at);
		CHECK(count != 0 && count <= OTA_CHUNK_BYTES);
		memcpy(&recovered[at], chunk, count);
		at += count;
	}
	CHECK(at == image_len);
	CHECK(memcmp(recovered, image, image_len) == 0);

	/* The CRC the query advertises must match what was downloaded, or
	 * a download has no way to know it is intact. */
	uint32_t length = 0, crc = 0;
	CHECK(app_image_info(&length, &crc));
	CHECK(length == image_len);
	CHECK(crc == crc32_compute(recovered, image_len));

	/* A read may not wander past the installed image into the erased
	 * remainder of the slot; the last one is clamped instead. */
	CHECK(send_read(image_len - 10u, OTA_CHUNK_BYTES, chunk, &off, &count) == OTA_STATUS_OK);
	CHECK(count == 10u);
	CHECK(send_read(image_len, 1, chunk, &off, &count) == OTA_STATUS_RANGE);
	CHECK(send_read(APP_SLOT_SIZE, 1, chunk, &off, &count) == OTA_STATUS_RANGE);
	CHECK(send_read(0, 0, chunk, &off, &count) == OTA_STATUS_RANGE);
	CHECK(send_read(0, OTA_CHUNK_BYTES + 1u, chunk, &off, &count) == OTA_STATUS_RANGE);

	/* A read during a transfer would be copying bytes that are being
	 * rewritten underneath it. */
	CHECK(send_begin(image_len) == OTA_STATUS_OK);
	CHECK(send_read(0, 16, chunk, &off, &count) == OTA_STATUS_STATE);

	printf("  the installed image downloads back byte-for-byte, CRC and all\n");
}

static void test_query_reports_what_is_installed(void)
{
	reset_world();
	reply_len = 0;
	uint8_t query[] = {0xf0, 0x47, 0x00, 0x7c, OTA_CMD, 0x00, 0x01,
	                   OTA_SUB_QUERY, 0xf7};

	/* Empty slot reports zero length, which is how recovery answers. */
	ota_handle_message(query, sizeof query, 0);
	CHECK(reply_len == 8u + 1u + OTA_QUERY_EXTRA + 1u);
	CHECK(read_digits(&reply[9 + 10], OTA_DIGITS_LENGTH) == 0);
	CHECK(read_digits(&reply[9 + 13], OTA_DIGITS_CRC) == 0);

	CHECK(upload(image, image_len, -1) == OTA_STATUS_OK);
	reply_len = 0;
	ota_handle_message(query, sizeof query, 0);
	CHECK(read_digits(&reply[9 + 10], OTA_DIGITS_LENGTH) == image_len);
	CHECK(read_digits(&reply[9 + 13], OTA_DIGITS_CRC) == crc32_compute(image, image_len));

	/* The older ten-byte prefix must be unchanged, so an ESP that only
	 * knows that much still reads the right slot and chunk sizes. */
	CHECK(reply[9 + 0] == OTA_PROTOCOL_VERSION);
	CHECK(read_digits(&reply[9 + 5], OTA_DIGITS_LENGTH) == APP_SLOT_SIZE);
	CHECK(read_digits(&reply[9 + 8], OTA_DIGITS_COUNT) == OTA_CHUNK_BYTES);
	printf("  the query reports the installed length and CRC, prefix unchanged\n");
}

static void test_real_image_if_present(void)
{
	FILE *f = fopen("../build/mpk-mini-open.bin", "rb");
	if (f == NULL) {
		printf("  (skipped: build/mpk-mini-open.bin not built)\n");
		return;
	}
	static uint8_t real[APP_SLOT_SIZE];
	size_t len = fread(real, 1, sizeof real, f);
	fclose(f);
	CHECK(len >= APP_MIN_SIZE && (len % 2) == 0);

	reset_world();
	CHECK(upload(real, (uint32_t)len, -1) == OTA_STATUS_OK);
	CHECK(app_image_valid());
	CHECK(app_image_length() == len);
	CHECK(memcmp(at(APP_SLOT_BASE), real, len) == 0);
	printf("  the real %zu-byte application image uploads and validates\n", len);

	/* tools/make_trailer.py builds the same 16 bytes in Python, using
	 * zlib's CRC-32, for flashing an image over SWD. If the two ever
	 * disagreed, a bench-flashed unit would silently sit in recovery. */
	f = fopen("../build/mpk-mini-open-trailer.bin", "rb");
	if (f == NULL) {
		printf("  (skipped: build/mpk-mini-open-trailer.bin not built)\n");
		return;
	}
	uint8_t from_tool[APP_TRAILER_SIZE];
	size_t tool_len = fread(from_tool, 1, sizeof from_tool, f);
	fclose(f);
	CHECK(tool_len == APP_TRAILER_SIZE);
	CHECK(memcmp(from_tool, at(APP_TRAILER_BASE), APP_TRAILER_SIZE) == 0);
	printf("  make_trailer.py produces the same trailer the keyboard writes\n");
}

int main(void)
{
	build_image();
	printf("firmware transfer:\n");
	test_sevenbit_roundtrip();
	test_crc32_known_vectors();
	test_complete_upload_boots();
	test_interrupted_upload_falls_back();
	test_corrupt_image_is_rejected();
	test_protocol_guards();
	test_flash_errors_surface();
	test_stalled_transfer_is_abandoned();
	test_boot_gate_rejects_nonsense();
	test_message_layer();
	test_download_round_trip();
	test_query_reports_what_is_installed();
	test_real_image_if_present();
	printf("all firmware-transfer tests passed\n");
	return 0;
}
