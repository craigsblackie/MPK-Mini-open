/*
 * End-to-end test for the firmware transfer, with both real ends.
 *
 * The sending side is the bridge's own firmware_update.c. The receiving
 * side is the keyboard's ota.c and app_image.c, compiled straight out of
 * ../../stm32/src, writing into an array that behaves like the STM32's
 * flash. Nothing between them is a reimplementation: the messages that
 * cross this harness are the bytes that would cross the UART, decoded by
 * the code that runs on the device.
 *
 * That is the point. The two ends could each be self-consistent and still
 * disagree -- about the 14-bit length field, the digit order, the 7-in-8
 * packing, which CRC -- and every one of those disagreements ends with a
 * keyboard sitting in recovery. Here they cost a failed assertion.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "crc32.h"
#include "otamap.h"
#include "otaproto.h"
#include "ota.h"
#include "app_image.h"
#include "sysex_bridge.h"

int delay_calls;      /* referenced by the shared FreeRTOS stub */
int log_warn_count;   /* referenced by the shared esp_log stub */
int64_t fake_us;      /* referenced by the shared esp_timer stub */
int task_notify_give_count;
void (*task_create_hook)(void (*entry)(void *), void *param);

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		exit(1); \
	} \
} while (0)

/* ---------- the keyboard's flash ---------- */

#define FAKE_BASE APP_SLOT_BASE
#define FAKE_SIZE (APP_TRAILER_BASE + FLASH_PAGE_SIZE - APP_SLOT_BASE)
static uint8_t fake_flash[FAKE_SIZE];

static uint8_t *at(uint32_t address)
{
	assert(address >= FAKE_BASE && address < FAKE_BASE + FAKE_SIZE);
	return &fake_flash[address - FAKE_BASE];
}

void flash_unlock(void) {}
void flash_lock(void) {}

bool flash_erase_page(uint32_t address)
{
	memset(at(address), 0xff, FLASH_PAGE_SIZE);
	return true;
}

bool flash_program(uint32_t address, const uint8_t *data, size_t len)
{
	uint8_t *dst = at(address);
	for (size_t i = 0; i < len; i++) {
		if (dst[i] != 0xffu && dst[i] != data[i]) return false; /* not erased */
		dst[i] &= data[i];
	}
	return memcmp(dst, data, len) == 0;
}

bool flash_is_erased(uint32_t address, size_t len)
{
	const uint8_t *p = at(address);
	for (size_t i = 0; i < len; i++) if (p[i] != 0xffu) return false;
	return true;
}

void flash_read(uint32_t address, void *dst, size_t len) { memcpy(dst, at(address), len); }
uint32_t flash_crc32(uint32_t address, size_t len) { return crc32_compute(at(address), len); }

static uint32_t keyboard_millis;
uint32_t systick_millis(void) { return keyboard_millis; }

static int keyboard_resets;
void ota_reset_system(void) { keyboard_resets++; }

/* ---------- the link ---------- */

/*
 * Stands in for sysex_bridge.c plus the UART plus the keyboard's
 * sysex.c: hand the request straight to the keyboard's real message
 * layer and hand its real reply back. `keyboard_online` models a unit
 * that is unplugged or not running firmware that answers.
 */
static bool keyboard_online = true;
/*
 * Models a keyboard running the application rather than recovery. The
 * real one answers BEGIN with OTA_STATUS_REBOOTING, invalidates itself
 * and resets; here the first BEGIN does that and the reset flips it into
 * recovery, so the bridge's retry has something that will accept the
 * transfer -- which is the behaviour being tested.
 */
static bool keyboard_in_app;
static uint8_t captured_reply[SYSEX_BRIDGE_MAX];
static size_t captured_len;
static unsigned messages_sent;
static size_t longest_request;

void ota_emit(const uint8_t *message, size_t len)
{
	CHECK(len <= sizeof captured_reply);
	/* Nothing between F0 and F7 may have its top bit set, or a real
	 * MIDI parser would treat it as the start of a new message. */
	for (size_t i = 1; i + 1 < len; i++) CHECK((message[i] & 0x80u) == 0);
	CHECK(message[0] == 0xf0 && message[len - 1] == 0xf7);
	memcpy(captured_reply, message, len);
	captured_len = len;
}

void midi_uart_send(const uint8_t *bytes, size_t len) { (void)bytes; (void)len; }

size_t sysex_bridge_request(const uint8_t *request, size_t request_len,
                            uint8_t expect_cmd, uint8_t *reply, size_t reply_cap,
                            uint32_t timeout_ms)
{
	(void)timeout_ms;
	messages_sent++;
	if (request_len > longest_request) longest_request = request_len;

	/* The same bound sysex.c enforces on the device: a request that
	 * does not fit its buffer would simply be dropped there. */
	CHECK(request_len <= 192);
	CHECK(request[0] == 0xf0 && request[request_len - 1] == 0xf7);
	for (size_t i = 1; i + 1 < request_len; i++) CHECK((request[i] & 0x80u) == 0);

	if (!keyboard_online) return 0;
	if (request[4] != expect_cmd) return 0;

	captured_len = 0;
	keyboard_millis += 50;

	if (keyboard_in_app) {
		uint8_t sub = request[7];
		if (sub == OTA_SUB_QUERY) {
			/* Same reply as recovery's, but saying "application". */
			ota_handle_message(request, request_len, request[2]);
			if (captured_len > 10) captured_reply[10] = OTA_MODE_APP;
		} else if (sub == OTA_SUB_BEGIN) {
			/* Hand over: give up bootability and restart. */
			flash_erase_page(APP_TRAILER_BASE);
			keyboard_in_app = false;
			keyboard_resets++;
			uint8_t reply_msg[] = {0xf0, 0x47, request[2], 0x7c, OTA_CMD,
			                       0x00, 0x02, OTA_SUB_BEGIN,
			                       OTA_STATUS_REBOOTING, 0xf7};
			ota_emit(reply_msg, sizeof reply_msg);
		} else {
			ota_handle_message(request, request_len, request[2]);
		}
	} else {
		ota_handle_message(request, request_len, request[2]);
	}

	size_t out = captured_len < reply_cap ? captured_len : reply_cap;
	memcpy(reply, captured_reply, out);
	return out;
}

bool sysex_bridge_keyboard_seen(void) { return keyboard_online; }
void sysex_bridge_init(void) {}

/* ---------- ESP-IDF fakes ---------- */

static const esp_partition_t slot_a = {.label = "ota_0", .size = 0x1E0000};
static const esp_partition_t slot_b = {.label = "ota_1", .size = 0x1E0000};
const esp_partition_t *fake_next_partition = &slot_b;
esp_err_t fake_ota_begin_result;
esp_err_t fake_ota_end_result;
uint8_t fake_ota_image[262144];
size_t fake_ota_written;
int fake_ota_aborted;
int fake_ota_boot_set;
int fake_restart_count;

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *from)
{ (void)from; return fake_next_partition; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &slot_a; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *h)
{ (void)p; (void)size; *h = 1; fake_ota_written = 0; return fake_ota_begin_result; }
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t len)
{
	(void)h;
	CHECK(fake_ota_written + len <= sizeof fake_ota_image);
	memcpy(fake_ota_image + fake_ota_written, data, len);
	fake_ota_written += len;
	return 0;
}
esp_err_t esp_ota_end(esp_ota_handle_t h) { (void)h; return fake_ota_end_result; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { (void)h; fake_ota_aborted++; return 0; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{ (void)p; fake_ota_boot_set++; return 0; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s)
{ (void)p; *s = ESP_OTA_IMG_VALID; return 0; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return 0; }
void esp_restart(void) { fake_restart_count++; }

/* The running partition's contents, for the bridge download. */
uint8_t fake_partition[262144];
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n)
{
	(void)p;
	if (off + n > sizeof fake_partition) return ESP_FAIL;
	memcpy(dst, fake_partition + off, n);
	return ESP_OK;
}

static const esp_app_desc_t fake_desc = {
	.version = "1.0.0", .date = "Jan  1 2026", .time = "00:00:00"
};
const esp_app_desc_t *esp_app_get_description(void) { return &fake_desc; }

/* The unit under test, included so the tests can reach what is static
 * in it -- the same way editor_test.c includes editor.c. */
#include "firmware_update.c"

/* ---------- HTTP plumbing the stub leaves to the test ---------- */

static char last_status[40];
static char last_body[4096];

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s)
{ (void)r; snprintf(last_status, sizeof last_status, "%s", s); return 0; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{ (void)r; snprintf(last_body, sizeof last_body, "%s", s); return 0; }
/* Captures whatever a handler sends as a file, in one piece or chunked. */
static uint8_t sent_body[262144];
static size_t sent_body_len;

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long len)
{
	(void)r;
	CHECK((size_t)len <= sizeof sent_body);
	memcpy(sent_body, buf, (size_t)len);
	sent_body_len = (size_t)len;
	return 0;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, long len)
{
	(void)r;
	if (buf == NULL || len == 0) return 0; /* end of the chunked response */
	CHECK(sent_body_len + (size_t)len <= sizeof sent_body);
	memcpy(sent_body + sent_body_len, buf, (size_t)len);
	sent_body_len += (size_t)len;
	return 0;
}

static const uint8_t *body_bytes;
static size_t body_left;
static size_t body_chunk_limit;   /* 0 means "as much as asked for" */

int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
	(void)r;
	if (body_left == 0) return 0;
	size_t take = len < body_left ? len : body_left;
	if (body_chunk_limit != 0 && take > body_chunk_limit) take = body_chunk_limit;
	memcpy(buf, body_bytes, take);
	body_bytes += take;
	body_left -= take;
	return (int)take;
}

/* ---------- a plausible keyboard image ---------- */

static uint8_t image[14068];
static const uint32_t image_len = sizeof image;

static void build_image(void)
{
	uint32_t stack = 0x20005000u;
	uint32_t entry = APP_SLOT_BASE + 0x97u;
	memcpy(&image[0], &stack, 4);
	memcpy(&image[4], &entry, 4);
	for (uint32_t i = 8; i < image_len; i++)
		image[i] = (uint8_t)(i * 131u + (i >> 7));
}

static void reset_world(void)
{
	memset(fake_flash, 0xff, sizeof fake_flash);
	keyboard_online = true;
	keyboard_in_app = false;
	keyboard_millis = 1000;
	keyboard_resets = 0;
	messages_sent = 0;
	longest_request = 0;
	fake_ota_begin_result = 0;
	fake_ota_end_result = 0;
	fake_ota_written = 0;
	fake_ota_aborted = 0;
	fake_ota_boot_set = 0;
	fake_restart_count = 0;
	body_chunk_limit = 0;
	sent_body_len = 0;
	last_status[0] = last_body[0] = '\0';
	task_create_hook = NULL;
	ota_init();
	firmware_update_init();
}

static void set_body(const void *data, size_t len)
{
	body_bytes = (const uint8_t *)data;
	body_left = len;
}

/* ---------- tests ---------- */

static void test_query_agrees(void)
{
	reset_world();
	struct keyboard_info info;
	CHECK(keyboard_query(&info));
	CHECK(info.present);
	CHECK(info.protocol == OTA_PROTOCOL_VERSION);
	CHECK(info.mode == OTA_MODE_RECOVERY);
	CHECK(info.slot_size == APP_SLOT_SIZE);
	CHECK(info.chunk_bytes == OTA_CHUNK_BYTES);
	printf("  the bridge reads back the slot size and chunk size the keyboard reports\n");
}

static void test_full_transfer(void)
{
	reset_world();
	CHECK(!app_image_valid());

	keyboard_push(image, image_len);

	CHECK(progress.phase == PHASE_DONE);
	CHECK(progress.done == image_len);
	CHECK(app_image_valid());
	CHECK(app_image_length() == image_len);
	CHECK(memcmp(at(APP_SLOT_BASE), image, image_len) == 0);

	/* Nothing in the exchange may exceed what the keyboard's SysEx
	 * buffer holds, and the transfer should not be wasting round trips. */
	CHECK(longest_request <= 192);
	unsigned expected_chunks = (image_len + OTA_CHUNK_BYTES - 1) / OTA_CHUNK_BYTES;
	CHECK(messages_sent == 1u + 1u + expected_chunks + 1u); /* query,begin,data,commit */
	printf("  %u bytes cross in %u messages and land byte-for-byte\n",
	       (unsigned)image_len, messages_sent);
}

static void test_offline_keyboard(void)
{
	reset_world();
	keyboard_online = false;
	keyboard_push(image, image_len);
	CHECK(progress.phase == PHASE_FAILED);
	CHECK(strstr(progress.detail, "did not answer") != NULL);
	CHECK(!app_image_valid());
	printf("  a keyboard that does not answer fails the update instead of hanging\n");
}

static void test_link_drops_mid_transfer(void)
{
	reset_world();
	keyboard_push(image, image_len);
	CHECK(app_image_valid());

	/* Now the link dies partway through a second update. The image
	 * that was installed is gone -- the slot is being rewritten -- but
	 * the trailer went first, so the keyboard comes up in recovery and
	 * can be uploaded to again rather than being dead. */
	unsigned drop_after = messages_sent;
	(void)drop_after;
	keyboard_online = true;
	messages_sent = 0;

	progress_start("keyboard", image_len);
	struct keyboard_info info;
	CHECK(keyboard_query(&info));
	uint8_t payload[OTA_DIGITS_OFFSET + OTA_CHUNK_ENCODED];
	write_digits(payload, OTA_DIGITS_OFFSET, image_len);
	CHECK(ota_exchange(OTA_SUB_BEGIN, payload, OTA_DIGITS_OFFSET,
	                   NULL, 0, NULL, REPLY_TIMEOUT_MS) == OTA_STATUS_OK);
	CHECK(!app_image_valid());

	for (int i = 0; i < 12; i++) {
		write_digits(payload, OTA_DIGITS_OFFSET, (uint32_t)i * OTA_CHUNK_BYTES);
		size_t encoded = sevenbit_encode(&image[i * OTA_CHUNK_BYTES],
		                                 OTA_CHUNK_BYTES, &payload[OTA_DIGITS_OFFSET]);
		CHECK(ota_exchange(OTA_SUB_DATA, payload, OTA_DIGITS_OFFSET + encoded,
		                   NULL, 0, NULL, REPLY_TIMEOUT_MS) == OTA_STATUS_OK);
		CHECK(!app_image_valid());
	}

	keyboard_online = false; /* the cable comes out here */
	CHECK(!app_image_valid());

	/* Plugged back in, the upload simply runs again. */
	keyboard_online = true;
	ota_init();
	keyboard_push(image, image_len);
	CHECK(progress.phase == PHASE_DONE);
	CHECK(app_image_valid());
	printf("  a link that drops mid-transfer leaves recovery, and retrying works\n");
}

static void test_wrong_file_is_refused(void)
{
	reset_world();
	keyboard_push(image, image_len);
	CHECK(app_image_valid());

	/* An ESP32 image offered to the keyboard: the likely mistake with
	 * two upload buttons on one page. The installed firmware must
	 * survive it untouched. */
	static uint8_t esp_image[4096];
	esp_image[0] = 0xe9;
	keyboard_push(esp_image, sizeof esp_image);
	CHECK(progress.phase == PHASE_FAILED);
	CHECK(strstr(progress.detail, "bridge image") != NULL);
	CHECK(app_image_valid());

	/* Too big for the slot, and not a vector table. */
	static uint8_t oversized[APP_SLOT_SIZE + 2];
	oversized[0] = 0x00; oversized[1] = 0x50; oversized[2] = 0x00; oversized[3] = 0x20;
	keyboard_push(oversized, sizeof oversized);
	CHECK(progress.phase == PHASE_FAILED);
	CHECK(app_image_valid());

	uint8_t nonsense[2048];
	memset(nonsense, 0x5a, sizeof nonsense);
	keyboard_push(nonsense, sizeof nonsense);
	CHECK(progress.phase == PHASE_FAILED);
	CHECK(strstr(progress.detail, "vector table") != NULL);
	CHECK(app_image_valid());
	printf("  a file for the wrong processor is refused before anything is erased\n");
}

/* Capture the worker rather than running it from inside the handler, so
 * the test can check that the request is answered before the transfer
 * starts -- which is the whole reason the transfer is on its own task. */
static void (*captured_entry)(void *);
static void *captured_param;

static void capture_task(void (*entry)(void *), void *param)
{
	captured_entry = entry;
	captured_param = param;
}

static void test_http_keyboard_upload(void)
{
	reset_world();

	httpd_req_t req = {.uri = "/api/firmware/keyboard", .content_len = image_len};
	set_body(image, image_len);
	body_chunk_limit = 700; /* TCP hands the body over in pieces */

	captured_entry = NULL;
	captured_param = NULL;
	task_create_hook = capture_task;

	CHECK(keyboard_post(&req) == 0);
	CHECK(strcmp(last_status, "202 Accepted") == 0);
	CHECK(strstr(last_body, "\"started\":true") != NULL);
	CHECK(progress.phase == PHASE_RUNNING);
	CHECK(captured_entry != NULL);

	captured_entry(captured_param);
	CHECK(progress.phase == PHASE_DONE);
	CHECK(app_image_valid());
	CHECK(memcmp(at(APP_SLOT_BASE), image, image_len) == 0);
	task_create_hook = NULL;
	printf("  the upload endpoint answers at once and finishes on its own task\n");
}

static void test_http_rejects_bad_upload(void)
{
	reset_world();
	static uint8_t esp_image[4096];
	esp_image[0] = 0xe9;

	httpd_req_t req = {.uri = "/api/firmware/keyboard", .content_len = sizeof esp_image};
	set_body(esp_image, sizeof esp_image);
	CHECK(keyboard_post(&req) == 0);
	CHECK(strncmp(last_status, "400", 3) == 0);
	CHECK(strstr(last_body, "bridge image") != NULL);
	CHECK(progress.phase != PHASE_RUNNING);

	/* Larger than the keyboard's slot: refused without being read in. */
	reset_world();
	httpd_req_t huge = {.uri = "/api/firmware/keyboard", .content_len = 1024 * 1024};
	CHECK(keyboard_post(&huge) == 0);
	CHECK(strncmp(last_status, "413", 3) == 0);
	printf("  the upload endpoint refuses the wrong file before staging it\n");
}

static void test_http_bridge_upload(void)
{
	reset_world();
	static uint8_t bridge_image[40000];
	bridge_image[0] = 0xe9;
	for (size_t i = 1; i < sizeof bridge_image; i++) bridge_image[i] = (uint8_t)i;

	httpd_req_t req = {.uri = "/api/firmware/bridge", .content_len = sizeof bridge_image};
	set_body(bridge_image, sizeof bridge_image);
	body_chunk_limit = 512;

	CHECK(bridge_post(&req) == 0);
	CHECK(progress.phase == PHASE_DONE);
	CHECK(fake_ota_written == sizeof bridge_image);
	CHECK(memcmp(fake_ota_image, bridge_image, sizeof bridge_image) == 0);
	CHECK(fake_ota_boot_set == 1);
	CHECK(fake_restart_count == 1);
	CHECK(fake_ota_aborted == 0);

	/* A keyboard image offered to the bridge is caught by its magic
	 * byte, and the spare slot is abandoned rather than left half
	 * written and marked bootable. */
	reset_world();
	httpd_req_t wrong = {.uri = "/api/firmware/bridge", .content_len = image_len};
	set_body(image, image_len);
	CHECK(bridge_post(&wrong) == 0);
	CHECK(progress.phase == PHASE_FAILED);
	CHECK(fake_ota_aborted == 1);
	CHECK(fake_ota_boot_set == 0);
	CHECK(fake_restart_count == 0);
	printf("  the bridge endpoint writes the spare slot and refuses a keyboard image\n");
}

/*
 * The download path, end to end: push an image in, pull it back out
 * through the HTTP handler, and require the bytes to be identical.
 *
 * This is the test that would catch a download that silently truncates,
 * or one that reads the whole 26 KiB slot instead of the installed
 * image -- both of which produce a file that looks plausible and does
 * not work.
 */
static void test_keyboard_download(void)
{
	reset_world();

	/* Nothing installed yet: refuse rather than hand over an erased slot. */
	httpd_req_t req = {.uri = "/api/firmware/keyboard"};
	CHECK(keyboard_download(&req) == 0);
	CHECK(strncmp(last_status, "502", 3) == 0);
	CHECK(strstr(last_body, "no application installed") != NULL);

	keyboard_push(image, image_len);
	CHECK(progress.phase == PHASE_DONE);
	ota_init(); /* the keyboard reboots after a commit */

	sent_body_len = 0;
	CHECK(keyboard_download(&req) == 0);
	if (sent_body_len != image_len)
		printf("    [download returned %zu bytes, status '%s', body '%s']\n",
		       sent_body_len, last_status, last_body);
	CHECK(sent_body_len == image_len);
	CHECK(memcmp(sent_body, image, image_len) == 0);
	printf("  an uploaded image downloads back byte-for-byte through HTTP\n");
}

static void test_download_reports_what_is_installed(void)
{
	reset_world();
	httpd_req_t req = {.uri = "/api/firmware"};

	CHECK(firmware_get(&req) == 0);
	CHECK(strstr(last_body, "\"canDownload\":false") != NULL);
	CHECK(strstr(last_body, "\"installedSize\":0") != NULL);

	keyboard_push(image, image_len);
	ota_init();
	CHECK(firmware_get(&req) == 0);
	CHECK(strstr(last_body, "\"canDownload\":true") != NULL);
	char expect[48];
	snprintf(expect, sizeof expect, "\"installedSize\":%u", (unsigned)image_len);
	CHECK(strstr(last_body, expect) != NULL);
	printf("  the status endpoint says whether there is anything to download\n");
}

/* The bridge serves its own image at its real length, not the whole
 * partition -- which is over half a megabyte of erased flash larger. */
static void test_bridge_download(void)
{
	reset_world();

	/* A two-segment image: header, two segment headers and payloads,
	 * padded to a 16-byte boundary with a checksum byte, then a hash. */
	memset(fake_partition, 0xff, sizeof fake_partition);
	esp_image_header_t header = {0};
	header.magic = ESP_IMAGE_HEADER_MAGIC;
	header.segment_count = 2;
	header.hash_appended = 1;
	memcpy(fake_partition, &header, sizeof header);

	uint32_t at = sizeof(esp_image_header_t);
	const uint32_t seg_len[2] = {4000, 2048};
	for (int i = 0; i < 2; i++) {
		esp_image_segment_header_t seg = {.load_addr = 0x40080000u, .data_len = seg_len[i]};
		memcpy(fake_partition + at, &seg, sizeof seg);
		at += sizeof seg;
		for (uint32_t b = 0; b < seg_len[i]; b++)
			fake_partition[at + b] = (uint8_t)(b + i);
		at += seg_len[i];
	}
	uint32_t expected = ((at + 16u) & ~0xfu) + 32u;

	httpd_req_t req = {.uri = "/api/firmware/bridge"};
	sent_body_len = 0;
	CHECK(bridge_download(&req) == 0);
	CHECK(sent_body_len == expected);
	CHECK(memcmp(sent_body, fake_partition, expected) == 0);
	CHECK(sent_body_len < 262144); /* not the whole partition */

	/* A partition that does not start with an image is refused rather
	 * than served as a file that cannot be flashed. */
	reset_world();
	memset(fake_partition, 0xff, sizeof fake_partition);
	CHECK(bridge_download(&req) == 0);
	CHECK(strncmp(last_status, "500", 3) == 0);
	printf("  the bridge serves its own image at its real %u-byte length\n",
	       (unsigned)expected);
}

/*
 * Updating a keyboard that is actually working.
 *
 * The application cannot rewrite the slot it runs from, so it hands the
 * transfer to recovery and restarts. That is the path every update to a
 * healthy unit takes, so the bridge has to follow it through rather than
 * reporting the handover as a failure -- which is precisely what it did
 * before this was handled, and what made uploads look broken while the
 * keyboard was playing.
 */
static void test_update_while_application_is_running(void)
{
	reset_world();
	keyboard_in_app = true;

	/* Something valid is installed and the unit is playing. */
	struct keyboard_info before;
	CHECK(keyboard_query(&before));
	CHECK(before.mode == OTA_MODE_APP);

	keyboard_push(image, image_len);

	CHECK(progress.phase == PHASE_DONE);
	CHECK(keyboard_resets >= 1);          /* it restarted into recovery */
	CHECK(app_image_valid());             /* and the new image landed */
	CHECK(memcmp(at(APP_SLOT_BASE), image, image_len) == 0);
	printf("  an update to a running keyboard goes through its restart into recovery\n");
}

static void test_status_endpoint(void)
{
	reset_world();
	httpd_req_t req = {.uri = "/api/firmware"};
	CHECK(firmware_get(&req) == 0);
	CHECK(strstr(last_body, "\"version\":\"1.0.0\"") != NULL);
	CHECK(strstr(last_body, "\"slot\":\"ota_0\"") != NULL);
	CHECK(strstr(last_body, "\"present\":true") != NULL);
	CHECK(strstr(last_body, "\"mode\":\"recovery\"") != NULL);

	char expected[64];
	snprintf(expected, sizeof expected, "\"slotSize\":%u", (unsigned)APP_SLOT_SIZE);
	CHECK(strstr(last_body, expected) != NULL);

	CHECK(strstr(last_body, "\"updating\":false") != NULL);

	keyboard_online = false;
	CHECK(firmware_get(&req) == 0);
	CHECK(strstr(last_body, "\"present\":false") != NULL);

	/* Mid-update the keyboard is not asked anything, because asking
	 * takes the lock the transfer is holding. */
	keyboard_online = true;
	progress_start("keyboard", 100);
	CHECK(firmware_get(&req) == 0);
	CHECK(strstr(last_body, "\"updating\":true") != NULL);
	firmware_update_init();

	CHECK(progress_get(&req) == 0);
	CHECK(strstr(last_body, "\"phase\":\"idle\"") != NULL);
	printf("  the status endpoint reports both sides, and a missing keyboard\n");
}

static void test_real_images_if_present(void)
{
	FILE *f = fopen("../../stm32/build/mpk-mini-open.bin", "rb");
	if (f == NULL) {
		printf("  (skipped: stm32/build/mpk-mini-open.bin not built)\n");
		return;
	}
	static uint8_t real[APP_SLOT_SIZE];
	size_t len = fread(real, 1, sizeof real, f);
	fclose(f);

	reset_world();
	keyboard_push(real, (uint32_t)len);
	CHECK(progress.phase == PHASE_DONE);
	CHECK(app_image_valid());
	CHECK(memcmp(at(APP_SLOT_BASE), real, len) == 0);
	printf("  the real %zu-byte application crosses the link and validates\n", len);
}

int main(void)
{
	build_image();
	printf("firmware update, bridge to keyboard:\n");
	test_query_agrees();
	test_full_transfer();
	test_offline_keyboard();
	test_link_drops_mid_transfer();
	test_wrong_file_is_refused();
	test_http_keyboard_upload();
	test_http_rejects_bad_upload();
	test_http_bridge_upload();
	test_status_endpoint();
	test_keyboard_download();
	test_download_reports_what_is_installed();
	test_bridge_download();
	test_update_while_application_is_running();
	test_real_images_if_present();
	printf("all firmware-update tests passed\n");
	return 0;
}
