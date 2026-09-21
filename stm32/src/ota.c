/*
 * Firmware transfer into the application slot -- receiving side of the
 * 'f' SysEx command. See otaproto.h for the wire format and otamap.h
 * for why the slot sits where it does.
 *
 * Everything here reaches flash through flash.h, and time and reset
 * through the two hooks below, so the whole state machine compiles and
 * runs on a host against a plain array standing in for the slot. That
 * matters more here than anywhere else in this firmware: the failure
 * mode being designed against is a unit that will not boot, which is
 * exactly the failure a bench test cannot iterate on quickly.
 */
#include "ota.h"
#include "otamap.h"
#include "otaproto.h"
#include "flash.h"
#include "crc32.h"
#include "sevenbit.h"
#include "app_image.h"
#include "systick.h"
#include "version.h"

/* Defined in reset.c on the target, stubbed by the host tests. */
void ota_reset_system(void);

/* A transfer that goes quiet is abandoned, so a browser tab closed
 * mid-upload does not leave the slot locked against the next attempt. */
#define OTA_IDLE_TIMEOUT_MS 5000u

/* Long enough for a 13-byte reply to clear both the USB endpoint and
 * the 31250-baud UART before the reset takes the link down. */
#define OTA_REBOOT_DELAY_MS 300u

enum { STATE_IDLE, STATE_RECEIVING, STATE_COMMITTED, STATE_REBOOTING };

static uint8_t state;
static uint32_t expected_len;
static uint32_t received_len;
static uint32_t erased_pages;   /* bit per page of the app slot */
static uint32_t last_activity;
static uint32_t reboot_at;

void ota_init(void)
{
	state = STATE_IDLE;
	expected_len = 0;
	received_len = 0;
	erased_pages = 0;
	last_activity = 0;
	reboot_at = 0;
}

uint32_t ota_received(void)
{
	return received_len;
}

bool ota_reboot_pending(void)
{
	return state == STATE_COMMITTED || state == STATE_REBOOTING;
}

/* Fixed-width 7-bit digits, most significant first -- the same shape the
 * stock protocol uses for its 14-bit length and tempo fields. */
static uint32_t read_digits(const uint8_t *digits, unsigned count)
{
	uint32_t value = 0;
	for (unsigned i = 0; i < count; i++) {
		value = (value << 7) | (uint32_t)(digits[i] & 0x7fu);
	}
	return value;
}

static bool erase_trailer(void)
{
	return flash_erase_page(APP_TRAILER_BASE);
}

/* Erase every page the range touches that has not been erased yet during
 * this transfer. Chunks do not align to pages, so this is driven by the
 * range rather than by the chunk boundary. */
static bool ensure_erased(uint32_t offset, uint32_t len)
{
	uint32_t first = offset / FLASH_PAGE_SIZE;
	uint32_t last = (offset + len - 1u) / FLASH_PAGE_SIZE;
	for (uint32_t page = first; page <= last; page++) {
		if (erased_pages & (1u << page)) continue;
		if (!flash_erase_page(APP_SLOT_BASE + page * FLASH_PAGE_SIZE))
			return false;
		erased_pages |= (1u << page);
	}
	return true;
}

uint8_t ota_begin(const uint8_t *payload, size_t len)
{
	if (len != OTA_DIGITS_OFFSET) return OTA_STATUS_RANGE;

	uint32_t length = read_digits(payload, OTA_DIGITS_OFFSET);
	if (length < APP_MIN_SIZE || length > APP_SLOT_SIZE || (length & 1u) != 0u)
		return OTA_STATUS_RANGE;

#ifndef RECOVERY_BUILD
	/*
	 * The application lives in the slot an update has to erase, and a
	 * Cortex-M3 cannot execute from flash while that flash is being
	 * erased -- the interface stalls the bus, and there would be
	 * nothing to come back to afterwards. So the application does not
	 * attempt the transfer at all. It erases its own trailer, which
	 * hands the decision to the loader, and restarts; the loader then
	 * finds no valid application and runs recovery, which lives in the
	 * resident region and can rewrite the slot safely.
	 *
	 * Giving up the installed image before the replacement has arrived
	 * is unavoidable: there is only one application slot, and 64 KiB of
	 * flash does not hold two. Recovery is the safety net that makes
	 * that acceptable -- the unit still enumerates, still answers, and
	 * the upload simply runs again.
	 */
	if (!erase_trailer()) return OTA_STATUS_FLASH;
	state = STATE_REBOOTING;
	reboot_at = systick_millis() + OTA_REBOOT_DELAY_MS;
	return OTA_STATUS_REBOOTING;
#else

	/* Invalidate before touching the image. From here until a COMMIT
	 * writes a fresh trailer, the loader sees no bootable app and stays
	 * in recovery -- which is exactly what should happen if the power
	 * fails halfway through what follows. */
	if (!erase_trailer()) return OTA_STATUS_FLASH;

	expected_len = length;
	received_len = 0;
	erased_pages = 0;
	state = STATE_RECEIVING;
	last_activity = systick_millis();
	return OTA_STATUS_OK;
#endif /* RECOVERY_BUILD */
}

uint8_t ota_data(const uint8_t *payload, size_t len, uint32_t *offset_out)
{
	*offset_out = received_len;
	if (state != STATE_RECEIVING) return OTA_STATUS_STATE;
	if (len <= OTA_DIGITS_OFFSET) return OTA_STATUS_RANGE;

	uint32_t offset = read_digits(payload, OTA_DIGITS_OFFSET);

	uint8_t raw[OTA_CHUNK_BYTES];
	size_t encoded_len = len - OTA_DIGITS_OFFSET;
	if (sevenbit_decoded_size(encoded_len) > sizeof raw) return OTA_STATUS_RANGE;
	size_t raw_len = sevenbit_decode(&payload[OTA_DIGITS_OFFSET], encoded_len, raw);
	if (raw_len == 0u) return OTA_STATUS_ENCODING;

	/* Strictly sequential. Retrying a chunk would mean reprogramming
	 * flash that has already been written, which NOR flash cannot do
	 * without another erase, so a sender that loses its place is told
	 * where the receiver actually is and starts the transfer again. */
	if (offset != received_len) return OTA_STATUS_SEQUENCE;
	if ((raw_len & 1u) != 0u) return OTA_STATUS_RANGE;
	if (offset + raw_len > expected_len) return OTA_STATUS_RANGE;

	if (!ensure_erased(offset, (uint32_t)raw_len)) return OTA_STATUS_FLASH;
	if (!flash_program(APP_SLOT_BASE + offset, raw, raw_len)) return OTA_STATUS_FLASH;

	received_len = offset + (uint32_t)raw_len;
	*offset_out = received_len;
	last_activity = systick_millis();
	return OTA_STATUS_OK;
}

uint8_t ota_commit(const uint8_t *payload, size_t len)
{
	if (state != STATE_RECEIVING) return OTA_STATUS_STATE;
	if (len != OTA_DIGITS_CRC) return OTA_STATUS_RANGE;
	if (received_len != expected_len) return OTA_STATUS_STATE;

	uint32_t expected_crc = read_digits(payload, OTA_DIGITS_CRC);
	if (flash_crc32(APP_SLOT_BASE, expected_len) != expected_crc) {
		/* The trailer is still erased, so the unit comes up in
		 * recovery and the upload can simply be repeated. */
		state = STATE_IDLE;
		return OTA_STATUS_CRC;
	}

	uint8_t trailer[APP_TRAILER_SIZE];
	uint32_t magic = APP_TRAILER_MAGIC;
	uint32_t inverse = ~expected_crc;
	for (unsigned i = 0; i < 4; i++) {
		trailer[i]      = (uint8_t)(magic >> (8u * i));
		trailer[4u + i] = (uint8_t)(expected_len >> (8u * i));
		trailer[8u + i] = (uint8_t)(expected_crc >> (8u * i));
		trailer[12u + i] = (uint8_t)(inverse >> (8u * i));
	}
	if (!flash_program(APP_TRAILER_BASE, trailer, sizeof trailer))
		return OTA_STATUS_FLASH;

	state = STATE_COMMITTED;
	reboot_at = systick_millis() + OTA_REBOOT_DELAY_MS;
	return OTA_STATUS_OK;
}

uint8_t ota_read(const uint8_t *payload, size_t len, uint8_t *out,
                 uint32_t *offset_out, uint32_t *count_out)
{
	*offset_out = 0;
	*count_out = 0;
	if (len != OTA_DIGITS_OFFSET + OTA_DIGITS_COUNT) return OTA_STATUS_RANGE;

	/* A transfer in flight is rewriting the very bytes a reader would
	 * be copying, so the two cannot overlap. */
	if (state != STATE_IDLE) return OTA_STATUS_STATE;

	uint32_t installed = 0, crc = 0;
	if (!app_image_info(&installed, &crc)) return OTA_STATUS_STATE;

	uint32_t offset = read_digits(payload, OTA_DIGITS_OFFSET);
	uint32_t count = read_digits(&payload[OTA_DIGITS_OFFSET], OTA_DIGITS_COUNT);
	if (count == 0u || count > OTA_CHUNK_BYTES) return OTA_STATUS_RANGE;
	if (offset >= installed) return OTA_STATUS_RANGE;
	if (offset + count > installed) count = installed - offset;

	flash_read(APP_SLOT_BASE + offset, out, count);
	*offset_out = offset;
	*count_out = count;
	return OTA_STATUS_OK;
}

uint8_t ota_abort(void)
{
	if (state == STATE_COMMITTED) return OTA_STATUS_STATE;
	if (!erase_trailer()) return OTA_STATUS_FLASH;
	state = STATE_IDLE;
	received_len = 0;
	expected_len = 0;
	erased_pages = 0;
	return OTA_STATUS_OK;
}

void ota_process(void)
{
	if (state == STATE_COMMITTED || state == STATE_REBOOTING) {
		if ((int32_t)(systick_millis() - reboot_at) >= 0) ota_reset_system();
		return;
	}
	if (state == STATE_RECEIVING &&
	    systick_millis() - last_activity > OTA_IDLE_TIMEOUT_MS) {
		state = STATE_IDLE;
		received_len = 0;
		expected_len = 0;
		erased_pages = 0;
	}
}

/* ---------- the 'f' message layer ---------- */

/*
 * Replies are deliberately short and always carry the sub-command and a
 * status byte, so the sender can tell an accepted step from a rejected
 * one without inferring anything from silence. A DATA reply also echoes
 * how many bytes have actually been committed, which is what lets a
 * sender that has lost its place find out where the receiver is.
 */
#define HEADER_LEN OTA_HEADER_LEN

static void send_reply(uint8_t id, uint8_t sub, uint8_t status,
                       const uint8_t *extra, uint8_t extra_len)
{
	/* Sized for the largest reply, which is a READ: the offset digits
	 * plus a whole 7-in-8 encoded chunk. */
	uint8_t message[HEADER_LEN + 2u + OTA_DIGITS_OFFSET + OTA_CHUNK_ENCODED + 1u];
	uint8_t payload_len = (uint8_t)(1u + extra_len);  /* status + extra */
	uint16_t length = (uint16_t)(1u + payload_len);   /* byte 7 counts too */

	message[0] = 0xF0;
	message[1] = 0x47;
	message[2] = id;
	message[3] = 0x7C;
	message[4] = OTA_CMD;
	message[5] = (uint8_t)((length >> 7) & 0x7fu);
	message[6] = (uint8_t)(length & 0x7fu);
	message[7] = sub;
	message[HEADER_LEN] = status;
	for (uint8_t i = 0; i < extra_len; i++) message[HEADER_LEN + 1u + i] = extra[i];
	message[HEADER_LEN + 1u + extra_len] = 0xF7;

	ota_emit(message, (size_t)(HEADER_LEN + 2u + extra_len));
}

/* ota.c's read_digits() in reverse. */
static void write_digits(uint8_t *out, unsigned count, uint32_t value)
{
	for (unsigned i = 0; i < count; i++)
		out[count - 1u - i] = (uint8_t)((value >> (7u * i)) & 0x7fu);
}

static void send_query(uint8_t id)
{
	uint8_t extra[OTA_QUERY_EXTRA];
	extra[0] = OTA_PROTOCOL_VERSION;
#ifdef RECOVERY_BUILD
	extra[1] = OTA_MODE_RECOVERY;
#else
	extra[1] = OTA_MODE_APP;
#endif
	extra[2] = FIRMWARE_VERSION_MAJOR;
	extra[3] = FIRMWARE_VERSION_MINOR;
	extra[4] = FIRMWARE_VERSION_PATCH;
	write_digits(&extra[5], OTA_DIGITS_LENGTH, APP_SLOT_SIZE);
	write_digits(&extra[8], OTA_DIGITS_COUNT, OTA_CHUNK_BYTES);

	/* What is installed, so the other end knows how much to ask for
	 * when downloading it and what the result should check out to.
	 * Both zero when nothing valid is installed. */
	uint32_t installed = 0, installed_crc = 0;
	(void)app_image_info(&installed, &installed_crc);
	write_digits(&extra[10], OTA_DIGITS_LENGTH, installed);
	write_digits(&extra[13], OTA_DIGITS_CRC, installed_crc);

	send_reply(id, OTA_SUB_QUERY, OTA_STATUS_OK, extra, sizeof extra);
}

void ota_handle_message(const uint8_t *message, size_t len, uint8_t id)
{
	/* Header, sub-command and the F7 at minimum. The declared length
	 * field is ignored in favour of what actually arrived: the payload
	 * is whatever sits between byte 7 and the terminator. */
	if (len < HEADER_LEN + 1u) return;

	uint8_t sub = message[7];
	const uint8_t *payload = &message[HEADER_LEN];
	size_t payload_len = len - HEADER_LEN - 1u;

	uint8_t status;
	switch (sub) {
	case OTA_SUB_QUERY:
		send_query(id);
		return;
	case OTA_SUB_BEGIN:
		status = ota_begin(payload, payload_len);
		break;
	case OTA_SUB_DATA: {
		uint32_t received = 0;
		status = ota_data(payload, payload_len, &received);
		uint8_t extra[OTA_DIGITS_OFFSET];
		write_digits(extra, OTA_DIGITS_OFFSET, received);
		send_reply(id, sub, status, extra, sizeof extra);
		return;
	}
	case OTA_SUB_COMMIT:
		status = ota_commit(payload, payload_len);
		break;
	case OTA_SUB_ABORT:
		status = ota_abort();
		break;
	case OTA_SUB_READ: {
		uint8_t raw[OTA_CHUNK_BYTES];
		uint32_t offset = 0, count = 0;
		status = ota_read(payload, payload_len, raw, &offset, &count);

		uint8_t extra[OTA_DIGITS_OFFSET + OTA_CHUNK_ENCODED];
		write_digits(extra, OTA_DIGITS_OFFSET, offset);
		size_t encoded = 0;
		if (status == OTA_STATUS_OK)
			encoded = sevenbit_encode(raw, count, &extra[OTA_DIGITS_OFFSET]);
		send_reply(id, sub, status, extra,
		           (uint8_t)(OTA_DIGITS_OFFSET + encoded));
		return;
	}
	default:
		status = OTA_STATUS_STATE;
		break;
	}
	send_reply(id, sub, status, 0, 0);
}
