/*
 * Firmware updates for both processors, driven from the editor portal.
 *
 * The keyboard half is the sending side of the 'f' SysEx command;
 * stm32/src/ota.c is the receiving side and
 * stm32/include/otaproto.h defines what passes between them. The
 * 7-in-8 codec and the CRC are compiled from the firmware's own sources
 * (see this component's CMakeLists.txt) rather than reimplemented here,
 * so the two ends cannot disagree about either.
 *
 * Two things shape the structure of this file.
 *
 * The HTTP server runs on a single task, so a request handler that sat
 * on the UART for the six seconds a 14 KB image takes at 31250 baud
 * would also stall the very endpoint the page polls to show progress.
 * The keyboard upload is therefore read into RAM, handed to a worker
 * task, and answered immediately; the page follows it through
 * /api/firmware/progress. The bridge's own image is far too large for
 * that (over a megabyte) but writes at flash speed rather than MIDI
 * speed, so it streams straight through and the page watches the
 * browser's own upload progress instead.
 *
 * And an update aimed at the wrong processor is the obvious mistake to
 * make with two upload buttons on one page, so both paths check the
 * first bytes of the file before erasing anything.
 */
#include "firmware_update.h"
#include "sysex_bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "crc32.h"
#include "otamap.h"
#include "otaproto.h"
#include "sevenbit.h"

static const char *TAG = "firmware";

/* Provided by main.c, which owns the UART. */
void midi_uart_send(const uint8_t *bytes, size_t len);

#define HEADER_LEN 8

/* A page erase on the STM32 takes tens of milliseconds and happens
 * inside whichever chunk first lands on a new page, on top of the 45 ms
 * the message itself takes to clock across at 31250 baud. */
#define REPLY_TIMEOUT_MS  800
/* A commit checksums the whole slot before programming the trailer. */
#define COMMIT_TIMEOUT_MS 3000

/* ESP-IDF application images start with this byte; STM32 images start
 * with a stack pointer in SRAM. Either one is enough to catch a file
 * offered to the wrong processor. */
#define ESP_IMAGE_MAGIC 0xe9u
#define STM32_SRAM_BASE 0x20000000u
#define STM32_SRAM_TOP  0x20005000u

/* The keyboard reports its own slot size in the query and the transfer
 * is checked against that, but a body has to be refused or staged in
 * RAM before any of that happens, so the layout's compile-time size is
 * the cap on what will even be read in. */
#define APP_UPLOAD_LIMIT APP_SLOT_SIZE

enum { PHASE_IDLE, PHASE_RUNNING, PHASE_DONE, PHASE_FAILED };

static struct {
	volatile int phase;
	const char *target;      /* "keyboard" or "bridge" */
	volatile uint32_t done;
	volatile uint32_t total;
	char detail[112];
} progress;

static void progress_start(const char *target, uint32_t total)
{
	progress.target = target;
	progress.done = 0;
	progress.total = total;
	progress.detail[0] = '\0';
	progress.phase = PHASE_RUNNING;
}

static void progress_finish(int phase, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(progress.detail, sizeof progress.detail, fmt, args);
	va_end(args);
	if (phase == PHASE_FAILED) ESP_LOGE(TAG, "%s", progress.detail);
	else ESP_LOGI(TAG, "%s", progress.detail);
	progress.phase = phase;
}

/* ---------- the keyboard's 'f' command ---------- */

static const char *status_text(int status)
{
	switch (status) {
	case OTA_STATUS_OK:       return "ok";
	case OTA_STATUS_STATE:    return "the keyboard was not expecting that step";
	case OTA_STATUS_RANGE:    return "the image does not fit the keyboard's slot";
	case OTA_STATUS_FLASH:    return "the keyboard reported a flash error";
	case OTA_STATUS_CRC:      return "the image arrived corrupted";
	case OTA_STATUS_ENCODING: return "the keyboard could not decode a chunk";
	case OTA_STATUS_SEQUENCE: return "a chunk arrived out of order";
	default:                  return "the keyboard reported an unknown error";
	}
}

static void write_digits(uint8_t *out, unsigned count, uint32_t value)
{
	for (unsigned i = 0; i < count; i++)
		out[count - 1u - i] = (uint8_t)((value >> (7u * i)) & 0x7fu);
}

static uint32_t read_digits(const uint8_t *digits, unsigned count)
{
	uint32_t value = 0;
	for (unsigned i = 0; i < count; i++)
		value = (value << 7) | (uint32_t)(digits[i] & 0x7fu);
	return value;
}

/*
 * One request/reply exchange. Returns the keyboard's status byte, or -1
 * if it did not answer, and copies any bytes it sent after the status
 * into `extra`.
 */
static int ota_exchange(uint8_t sub, const uint8_t *payload, size_t payload_len,
                        uint8_t *extra, size_t extra_cap, size_t *extra_len,
                        uint32_t timeout_ms)
{
	uint8_t message[HEADER_LEN + OTA_DIGITS_OFFSET + OTA_CHUNK_ENCODED + 1];
	if (HEADER_LEN + payload_len + 1u > sizeof message) return -1;

	uint16_t length = (uint16_t)(1u + payload_len); /* byte 7 counts too */
	message[0] = 0xf0;
	message[1] = 0x47;
	message[2] = 0x00;
	message[3] = 0x7c;
	message[4] = OTA_CMD;
	message[5] = (uint8_t)((length >> 7) & 0x7fu);
	message[6] = (uint8_t)(length & 0x7fu);
	message[7] = sub;
	if (payload_len != 0) memcpy(&message[HEADER_LEN], payload, payload_len);
	message[HEADER_LEN + payload_len] = 0xf7;

	uint8_t reply[SYSEX_BRIDGE_MAX];
	size_t len = sysex_bridge_request(message, HEADER_LEN + payload_len + 1u,
	                                  OTA_CMD, reply, sizeof reply, timeout_ms);
	if (len < HEADER_LEN + 2u) return -1;     /* header + status + F7 */
	if (reply[7] != sub) return -1;           /* an answer to something else */

	size_t available = len - HEADER_LEN - 2u; /* after the status, before F7 */
	if (extra_len != NULL) {
		size_t copy = available < extra_cap ? available : extra_cap;
		if (extra != NULL && copy != 0) memcpy(extra, &reply[HEADER_LEN + 1u], copy);
		*extra_len = copy;
	}
	return reply[HEADER_LEN];
}

struct keyboard_info {
	bool present;
	uint8_t protocol;
	uint8_t mode;             /* OTA_MODE_APP or OTA_MODE_RECOVERY */
	uint8_t version[3];
	uint32_t slot_size;
	uint32_t chunk_bytes;
	uint32_t installed_len;   /* 0 when nothing valid is installed */
	uint32_t installed_crc;
	bool can_download;        /* the reply carried the newer fields */
};

static bool keyboard_query(struct keyboard_info *out)
{
	memset(out, 0, sizeof *out);

	/* Must hold the whole query reply; a short buffer would silently
	 * drop the installed-image fields at the end of it. */
	uint8_t extra[OTA_QUERY_EXTRA];
	size_t extra_len = 0;
	int status = ota_exchange(OTA_SUB_QUERY, NULL, 0, extra, sizeof extra,
	                          &extra_len, REPLY_TIMEOUT_MS);
	if (status != OTA_STATUS_OK || extra_len < OTA_QUERY_EXTRA_V1) return false;

	out->present = true;
	out->protocol = extra[0];
	out->mode = extra[1];
	out->version[0] = extra[2];
	out->version[1] = extra[3];
	out->version[2] = extra[4];
	out->slot_size = read_digits(&extra[5], OTA_DIGITS_LENGTH);
	out->chunk_bytes = read_digits(&extra[8], OTA_DIGITS_COUNT);

	/* The installed image's length and CRC are a later addition. An
	 * older keyboard simply does not send them, which is reported as
	 * "cannot download" rather than treated as a broken reply -- the
	 * resident recovery image can never be updated in the field, so
	 * this has to keep working against one that predates the feature. */
	if (extra_len >= OTA_QUERY_EXTRA) {
		out->installed_len = read_digits(&extra[10], OTA_DIGITS_LENGTH);
		out->installed_crc = read_digits(&extra[13], OTA_DIGITS_CRC);
		out->can_download = out->installed_len != 0;
	}
	return true;
}

/* Reject a file that cannot be an STM32 application before the keyboard
 * erases anything. The resident loader applies the same two tests to
 * the slot before branching into it (stm32/src/app_image.c), so
 * anything that fails here would have ended up in recovery anyway. */
static const char *keyboard_image_problem(const uint8_t *image, uint32_t len,
                                          uint32_t slot_size)
{
	if (len < 512u || (len & 1u) != 0u) return "that file is not a valid keyboard image";
	if (len > slot_size) return "that file is too large for the keyboard's slot";
	if (image[0] == ESP_IMAGE_MAGIC) return "that looks like a bridge image, not a keyboard image";

	uint32_t stack = (uint32_t)image[0] | ((uint32_t)image[1] << 8) |
	                 ((uint32_t)image[2] << 16) | ((uint32_t)image[3] << 24);
	if (stack < STM32_SRAM_BASE || stack > STM32_SRAM_TOP)
		return "that file does not start with a keyboard vector table";
	return NULL;
}

/* Drive a whole transfer. Reports through progress_finish() rather than
 * returning, because it runs on its own task well after the HTTP request
 * that started it has been answered. */
static void keyboard_push(const uint8_t *image, uint32_t len)
{
	struct keyboard_info info;
	if (!keyboard_query(&info)) {
		progress_finish(PHASE_FAILED, "the keyboard did not answer");
		return;
	}
	if (info.protocol != OTA_PROTOCOL_VERSION) {
		progress_finish(PHASE_FAILED,
		                "the keyboard speaks transfer protocol %u, this bridge speaks %u",
		                info.protocol, OTA_PROTOCOL_VERSION);
		return;
	}

	const char *problem = keyboard_image_problem(image, len, info.slot_size);
	if (problem != NULL) {
		progress_finish(PHASE_FAILED, "%s", problem);
		return;
	}

	uint32_t chunk_bytes = info.chunk_bytes;
	if (chunk_bytes == 0 || chunk_bytes > OTA_CHUNK_BYTES) chunk_bytes = OTA_CHUNK_BYTES;

	uint8_t payload[OTA_DIGITS_OFFSET + OTA_CHUNK_ENCODED];
	write_digits(payload, OTA_DIGITS_OFFSET, len);
	int status = ota_exchange(OTA_SUB_BEGIN, payload, OTA_DIGITS_OFFSET,
	                          NULL, 0, NULL, REPLY_TIMEOUT_MS);
	if (status != OTA_STATUS_OK) {
		progress_finish(PHASE_FAILED, "%s",
		                status < 0 ? "the keyboard did not answer" : status_text(status));
		return;
	}

	for (uint32_t offset = 0; offset < len; ) {
		uint32_t remaining = len - offset;
		uint32_t take = remaining < chunk_bytes ? remaining : chunk_bytes;

		write_digits(payload, OTA_DIGITS_OFFSET, offset);
		size_t encoded = sevenbit_encode(&image[offset], take,
		                                 &payload[OTA_DIGITS_OFFSET]);

		uint8_t echo[OTA_DIGITS_OFFSET];
		size_t echo_len = 0;
		status = ota_exchange(OTA_SUB_DATA, payload, OTA_DIGITS_OFFSET + encoded,
		                      echo, sizeof echo, &echo_len, REPLY_TIMEOUT_MS);
		if (status != OTA_STATUS_OK) {
			progress_finish(PHASE_FAILED, "%s",
			                status < 0 ? "the keyboard stopped answering mid-transfer"
			                           : status_text(status));
			ota_exchange(OTA_SUB_ABORT, NULL, 0, NULL, 0, NULL, REPLY_TIMEOUT_MS);
			return;
		}
		if (echo_len == OTA_DIGITS_OFFSET &&
		    read_digits(echo, OTA_DIGITS_OFFSET) != offset + take) {
			progress_finish(PHASE_FAILED,
			                "the keyboard and the bridge disagree about how much arrived");
			ota_exchange(OTA_SUB_ABORT, NULL, 0, NULL, 0, NULL, REPLY_TIMEOUT_MS);
			return;
		}

		offset += take;
		progress.done = offset;
	}

	write_digits(payload, OTA_DIGITS_CRC, crc32_compute(image, len));
	status = ota_exchange(OTA_SUB_COMMIT, payload, OTA_DIGITS_CRC,
	                      NULL, 0, NULL, COMMIT_TIMEOUT_MS);
	if (status != OTA_STATUS_OK) {
		progress_finish(PHASE_FAILED, "%s",
		                status < 0 ? "the keyboard did not confirm the update"
		                           : status_text(status));
		return;
	}

	progress_finish(PHASE_DONE, "keyboard updated to %u bytes; it is restarting",
	                (unsigned)len);
}

/*
 * Read the installed application back out of the keyboard, into a buffer
 * the caller owns. Returns the number of bytes recovered, or 0.
 *
 * This is the counterpart to keyboard_push(): the same link, the same
 * chunking, the other direction. It exists so the image actually running
 * on a unit can be copied off it without a debugger -- which is the only
 * way to take a backup of a build whose sources have moved on.
 */
static uint32_t keyboard_pull(uint8_t *out, uint32_t capacity, const char **error)
{
	struct keyboard_info info;
	if (!keyboard_query(&info)) {
		*error = "the keyboard did not answer";
		return 0;
	}
	if (!info.can_download) {
		*error = info.installed_len == 0
			? "the keyboard has no application installed to download"
			: "this keyboard's firmware is too old to send one back";
		return 0;
	}
	if (info.installed_len > capacity) {
		*error = "the installed image is larger than expected";
		return 0;
	}

	uint32_t chunk_bytes = info.chunk_bytes;
	if (chunk_bytes == 0 || chunk_bytes > OTA_CHUNK_BYTES) chunk_bytes = OTA_CHUNK_BYTES;

	uint8_t payload[OTA_DIGITS_OFFSET + OTA_DIGITS_COUNT];
	uint8_t reply[OTA_DIGITS_OFFSET + OTA_CHUNK_ENCODED];

	for (uint32_t offset = 0; offset < info.installed_len; ) {
		uint32_t remaining = info.installed_len - offset;
		uint32_t want = remaining < chunk_bytes ? remaining : chunk_bytes;

		write_digits(payload, OTA_DIGITS_OFFSET, offset);
		write_digits(&payload[OTA_DIGITS_OFFSET], OTA_DIGITS_COUNT, want);

		size_t reply_len = 0;
		int status = ota_exchange(OTA_SUB_READ, payload, sizeof payload,
		                          reply, sizeof reply, &reply_len, REPLY_TIMEOUT_MS);
		if (status != OTA_STATUS_OK) {
			*error = status < 0 ? "the keyboard stopped answering mid-download"
			                    : status_text(status);
			return 0;
		}
		if (reply_len <= OTA_DIGITS_OFFSET) {
			*error = "the keyboard sent an empty chunk";
			return 0;
		}
		if (read_digits(reply, OTA_DIGITS_OFFSET) != offset) {
			*error = "the keyboard sent the wrong part of the image";
			return 0;
		}

		uint8_t raw[OTA_CHUNK_BYTES];
		size_t got = sevenbit_decode(&reply[OTA_DIGITS_OFFSET],
		                             reply_len - OTA_DIGITS_OFFSET, raw);
		if (got == 0 || offset + got > info.installed_len) {
			*error = "the keyboard sent a chunk that could not be decoded";
			return 0;
		}
		memcpy(&out[offset], raw, got);
		offset += (uint32_t)got;
		progress.done = offset;
	}

	/* The keyboard told us what this should check out to, so verify it
	 * rather than handing over a file that might be short a byte. */
	if (crc32_compute(out, info.installed_len) != info.installed_crc) {
		*error = "the downloaded image did not match the keyboard's checksum";
		return 0;
	}

	*error = NULL;
	return info.installed_len;
}

struct keyboard_job {
	uint8_t *image;
	uint32_t len;
};

static void keyboard_push_task(void *param)
{
	struct keyboard_job *job = (struct keyboard_job *)param;
	keyboard_push(job->image, job->len);
	free(job->image);
	free(job);
	vTaskDelete(NULL);
}

/* ---------- HTTP ---------- */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
	char body[192];
	snprintf(body, sizeof body, "{\"error\":\"%s\"}", message);
	httpd_resp_set_status(req, status);
	return send_json(req, body);
}

static esp_err_t firmware_get(httpd_req_t *req)
{
	const esp_app_desc_t *running = esp_app_get_description();
	const esp_partition_t *slot = esp_ota_get_running_partition();

	/* Asking the keyboard anything takes the bridge's request lock, and
	 * during a transfer that lock is held for most of every chunk. A
	 * page reloaded mid-update should answer immediately with what is
	 * already known rather than stalling behind the transfer. */
	struct keyboard_info info = {0};
	bool updating = progress.phase == PHASE_RUNNING;
	bool present = updating ? false : keyboard_query(&info);

	char body[460];
	int n = snprintf(body, sizeof body,
		"{\"updating\":%s,"
		"\"bridge\":{\"version\":\"%s\",\"built\":\"%s %s\",\"slot\":\"%s\"},",
		updating ? "true" : "false",
		running->version, running->date, running->time,
		slot != NULL ? slot->label : "?");
	if (present) {
		snprintf(body + n, sizeof body - (size_t)n,
			"\"keyboard\":{\"present\":true,\"mode\":\"%s\","
			"\"version\":\"%u.%u.%u\",\"slotSize\":%u,\"protocol\":%u,"
			"\"installedSize\":%u,\"canDownload\":%s}}",
			info.mode == OTA_MODE_RECOVERY ? "recovery" : "app",
			info.version[0], info.version[1], info.version[2],
			(unsigned)info.slot_size, info.protocol,
			(unsigned)info.installed_len, info.can_download ? "true" : "false");
	} else {
		snprintf(body + n, sizeof body - (size_t)n, "\"keyboard\":{\"present\":false}}");
	}
	return send_json(req, body);
}

static esp_err_t progress_get(httpd_req_t *req)
{
	static const char *names[] = {"idle", "running", "done", "failed"};
	char body[224];
	snprintf(body, sizeof body,
		"{\"phase\":\"%s\",\"target\":\"%s\",\"done\":%u,\"total\":%u,\"detail\":\"%s\"}",
		names[progress.phase], progress.target != NULL ? progress.target : "",
		(unsigned)progress.done, (unsigned)progress.total, progress.detail);
	return send_json(req, body);
}

static esp_err_t keyboard_post(httpd_req_t *req)
{
	if (progress.phase == PHASE_RUNNING)
		return send_error(req, "409 Conflict", "an update is already running");
	if (req->content_len == 0 || req->content_len > APP_UPLOAD_LIMIT)
		return send_error(req, "413 Payload Too Large", "that is not a keyboard image");

	uint32_t len = (uint32_t)req->content_len;
	uint8_t *image = malloc(len);
	if (image == NULL)
		return send_error(req, "507 Insufficient Storage", "not enough memory to stage the image");

	uint32_t got = 0;
	while (got < len) {
		int n = httpd_req_recv(req, (char *)image + got, len - got);
		if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
		if (n <= 0) {
			free(image);
			return send_error(req, "400 Bad Request", "the upload ended early");
		}
		got += (uint32_t)n;
	}

	/* Check before starting anything, so an obvious mistake is reported
	 * in the reply to this request rather than in a progress poll. */
	const char *problem = keyboard_image_problem(image, len, APP_UPLOAD_LIMIT);
	if (problem != NULL) {
		free(image);
		return send_error(req, "400 Bad Request", problem);
	}

	struct keyboard_job *job = malloc(sizeof *job);
	if (job == NULL) {
		free(image);
		return send_error(req, "507 Insufficient Storage", "not enough memory to start");
	}
	job->image = image;
	job->len = len;

	progress_start("keyboard", len);
	/* Below the UART task's priority: the transfer is paced by the
	 * keyboard's replies, and MIDI must keep flowing while it runs. */
	if (xTaskCreate(keyboard_push_task, "kbd-update", 4096, job, 5, NULL) != pdPASS) {
		free(image);
		free(job);
		progress_finish(PHASE_FAILED, "could not start the update task");
		return send_error(req, "500 Internal Server Error", progress.detail);
	}

	httpd_resp_set_status(req, "202 Accepted");
	return send_json(req, "{\"started\":true}");
}

static esp_err_t bridge_post(httpd_req_t *req)
{
	if (progress.phase == PHASE_RUNNING)
		return send_error(req, "409 Conflict", "an update is already running");
	if (req->content_len == 0)
		return send_error(req, "400 Bad Request", "no file was uploaded");

	const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
	if (target == NULL)
		return send_error(req, "500 Internal Server Error", "no spare slot to write to");

	progress_start("bridge", (uint32_t)req->content_len);

	esp_ota_handle_t handle = 0;
	esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
	if (err != ESP_OK) {
		progress_finish(PHASE_FAILED, "could not open the spare slot (%s)",
		                esp_err_to_name(err));
		return send_error(req, "500 Internal Server Error", progress.detail);
	}

	uint8_t buffer[1024];
	bool checked_magic = false;
	while (progress.done < progress.total) {
		int n = httpd_req_recv(req, (char *)buffer, sizeof buffer);
		if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
		if (n <= 0) {
			esp_ota_abort(handle);
			progress_finish(PHASE_FAILED, "the upload ended early");
			return send_error(req, "400 Bad Request", progress.detail);
		}
		if (!checked_magic) {
			if (buffer[0] != ESP_IMAGE_MAGIC) {
				esp_ota_abort(handle);
				progress_finish(PHASE_FAILED,
				                "that does not look like a bridge image");
				return send_error(req, "400 Bad Request", progress.detail);
			}
			checked_magic = true;
		}
		err = esp_ota_write(handle, buffer, (size_t)n);
		if (err != ESP_OK) {
			esp_ota_abort(handle);
			progress_finish(PHASE_FAILED, "write failed (%s)", esp_err_to_name(err));
			return send_error(req, "500 Internal Server Error", progress.detail);
		}
		progress.done += (uint32_t)n;
	}

	err = esp_ota_end(handle);
	if (err != ESP_OK) {
		progress_finish(PHASE_FAILED, "the image failed its own checks (%s)",
		                esp_err_to_name(err));
		return send_error(req, "400 Bad Request", progress.detail);
	}
	err = esp_ota_set_boot_partition(target);
	if (err != ESP_OK) {
		progress_finish(PHASE_FAILED, "could not select the new slot (%s)",
		                esp_err_to_name(err));
		return send_error(req, "500 Internal Server Error", progress.detail);
	}

	progress_finish(PHASE_DONE, "bridge updated; it is restarting");
	esp_err_t sent = send_json(req, "{\"ok\":true,\"restarting\":true}");

	/* Let the reply reach the browser before the radio goes away
	 * underneath it. The new image boots on probation and reverts if it
	 * never reaches esp_ota_mark_app_valid_cancel_rollback(). */
	vTaskDelay(pdMS_TO_TICKS(400));
	esp_restart();
	return sent;
}

/*
 * The exact length of the image in a partition.
 *
 * The partition is 1.875 MB and the image is about 1.2 MB, so serving
 * the whole thing would hand over half a megabyte of erased flash and a
 * file that does not match what was flashed. An ESP32 image describes
 * its own extent: a header, then one segment header and payload each,
 * then a padding byte run to a 16-byte boundary with a checksum byte
 * last, then an optional appended SHA-256. Walking that gives the real
 * size, so the download is byte-identical to the build it came from.
 */
static bool image_length(const esp_partition_t *part, uint32_t *out)
{
	esp_image_header_t header;
	if (esp_partition_read(part, 0, &header, sizeof header) != ESP_OK) return false;
	if (header.magic != ESP_IMAGE_HEADER_MAGIC) return false;

	uint32_t at = sizeof(esp_image_header_t);
	for (unsigned i = 0; i < header.segment_count; i++) {
		esp_image_segment_header_t segment;
		if (esp_partition_read(part, at, &segment, sizeof segment) != ESP_OK) return false;
		at += sizeof segment;
		if (segment.data_len > part->size || at + segment.data_len > part->size)
			return false;
		at += segment.data_len;
	}

	/* One checksum byte, positioned so the total is a multiple of 16. */
	at = (at + 16u) & ~0xfu;
	if (header.hash_appended) at += 32u;
	if (at > part->size) return false;

	*out = at;
	return true;
}

static esp_err_t send_binary(httpd_req_t *req, const char *filename,
                             const uint8_t *data, uint32_t len)
{
	char disposition[96];
	snprintf(disposition, sizeof disposition, "attachment; filename=\"%s\"", filename);
	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Content-Disposition", disposition);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, (const char *)data, (ssize_t)len);
}

static esp_err_t keyboard_download(httpd_req_t *req)
{
	if (progress.phase == PHASE_RUNNING)
		return send_error(req, "409 Conflict", "an update is already running");

	uint8_t *image = malloc(APP_UPLOAD_LIMIT);
	if (image == NULL)
		return send_error(req, "507 Insufficient Storage", "not enough memory to stage the image");

	progress_start("keyboard download", 0);
	const char *error = NULL;
	uint32_t len = keyboard_pull(image, APP_UPLOAD_LIMIT, &error);
	if (len == 0) {
		free(image);
		progress_finish(PHASE_FAILED, "%s", error != NULL ? error : "download failed");
		return send_error(req, "502 Bad Gateway", progress.detail);
	}
	progress_finish(PHASE_DONE, "downloaded %u bytes from the keyboard", (unsigned)len);

	esp_err_t err = send_binary(req, "mpk-mini-open.bin", image, len);
	free(image);
	return err;
}

static esp_err_t bridge_download(httpd_req_t *req)
{
	const esp_partition_t *running = esp_ota_get_running_partition();
	if (running == NULL)
		return send_error(req, "500 Internal Server Error", "no running partition");

	uint32_t len = 0;
	if (!image_length(running, &len))
		return send_error(req, "500 Internal Server Error", "could not measure the running image");

	/* Over a megabyte, so this streams rather than being staged in
	 * RAM the bridge does not have. */
	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Content-Disposition",
	                   "attachment; filename=\"mpk_mini_ble_midi.bin\"");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");

	uint8_t buffer[1024];
	for (uint32_t at = 0; at < len; ) {
		uint32_t take = len - at;
		if (take > sizeof buffer) take = sizeof buffer;
		if (esp_partition_read(running, at, buffer, take) != ESP_OK) {
			httpd_resp_send_chunk(req, NULL, 0); /* end it; the length is short */
			return ESP_FAIL;
		}
		if (httpd_resp_send_chunk(req, (const char *)buffer, (ssize_t)take) != ESP_OK)
			return ESP_FAIL;
		at += take;
	}
	return httpd_resp_send_chunk(req, NULL, 0);
}

static const httpd_uri_t routes[] = {
	{.uri = "/api/firmware",          .method = HTTP_GET,  .handler = firmware_get},
	{.uri = "/api/firmware/progress", .method = HTTP_GET,  .handler = progress_get},
	{.uri = "/api/firmware/keyboard", .method = HTTP_POST, .handler = keyboard_post},
	{.uri = "/api/firmware/bridge",   .method = HTTP_POST, .handler = bridge_post},
	{.uri = "/api/firmware/keyboard", .method = HTTP_GET,  .handler = keyboard_download},
	{.uri = "/api/firmware/bridge",   .method = HTTP_GET,  .handler = bridge_download},
};

_Static_assert(sizeof routes / sizeof routes[0] == FIRMWARE_UPDATE_ROUTE_COUNT,
               "FIRMWARE_UPDATE_ROUTE_COUNT must match the route table");

void firmware_update_register_routes(httpd_handle_t server)
{
	for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
		httpd_register_uri_handler(server, &routes[i]);
}

void firmware_update_init(void)
{
	memset(&progress, 0, sizeof progress);
	progress.phase = PHASE_IDLE;
	progress.target = "";
}
