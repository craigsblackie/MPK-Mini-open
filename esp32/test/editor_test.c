/*
 * Host tests for the editor's SysEx translation.
 *
 * editor.c duplicates the original firmware's RECORD_TO_WIRE reorder table
 * so it can address program fields by record offset. That duplication is
 * the real risk in this file, so the first test compares it byte for byte
 * against stm32/src/program.c's copy, read from source at build time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_http_server.h"
#include "captive_dns.h"

/* ---- semaphore + bridge fakes, driven by the test ---- */
static int mutex_obj, binary_obj;
static int reply_available;
int delay_calls;      /* referenced by the shared FreeRTOS stub */
int log_warn_count;   /* referenced by the shared esp_log stub */

/* Stands in for the page the build embeds from editor.html. */
const uint8_t editor_html_start[] = "<!doctype html><title>t</title>";
const uint8_t editor_html_end[] = {0};

captive_dns_handle_t captive_dns_start(uint32_t ap_addr)
{ (void)ap_addr; return (captive_dns_handle_t)1; }
void captive_dns_stop(captive_dns_handle_t handle) { (void)handle; }

/* editor.c hands the portal's HTTP server to the firmware-update panel.
 * That panel has its own suite (firmware_update_test.c), which runs it
 * against the keyboard's real receiving code; these tests are about the
 * program and settings translation, so it is stubbed out here. */
void firmware_update_init(void) {}
void firmware_update_register_routes(httpd_handle_t server) { (void)server; }

#include "editor.c"

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_obj; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &binary_obj; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) {
	(void)t;
	if (s == &binary_obj) { if (!reply_available) return 0; reply_available = 0; return 1; }
	return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s) { if (s == &binary_obj) reply_available = 1; return 1; }

/* ---- captured UART traffic ---- */
static uint8_t uart_tx[512];
static size_t uart_tx_len;
static void kb_apply_write(const uint8_t *m, size_t n);

/* The real keyboard applies an 'a' write as the bytes arrive, and
 * store_program reads back to confirm it before returning -- so the fake
 * has to apply it here, not after the handler returns, or the read-back
 * legitimately fails. */
void midi_uart_send(const uint8_t *b, size_t n)
{
	if (uart_tx_len + n <= sizeof(uart_tx)) { memcpy(uart_tx + uart_tx_len, b, n); uart_tx_len += n; }
	kb_apply_write(b, n);
}

/* ---- the fake keyboard: a program store that answers SysEx ---- */
static uint8_t kb_wire[5][RECORD_SIZE];
static uint8_t kb_settings[SETTINGS_LEN] = {0, 0, 100, 100};
static bool kb_online = true;

size_t sysex_bridge_request(const uint8_t *req, size_t req_len, uint8_t expect,
                            uint8_t *reply, size_t cap, uint32_t timeout)
{
	(void)timeout;
	midi_uart_send(req, req_len);
	if (!kb_online) return 0;
	uint8_t cmd = req[4], arg = req[7];
	if (cmd == 'c' && expect == 'c' && arg < 5) {
		uint8_t m[PROGRAM_MSG];
		m[0]=0xf0;m[1]=0x47;m[2]=0;m[3]=0x7c;m[4]='c';m[5]=0;m[6]=0x66;m[7]=arg;
		memcpy(&m[HEADER_LEN], kb_wire[arg], RECORD_SIZE);
		m[PROGRAM_MSG-1]=0xf7;
		size_t n = sizeof(m) < cap ? sizeof(m) : cap;
		memcpy(reply, m, n); return n;
	}
	if (cmd == 'v' && expect == 'v') {
		if (arg == 1) memcpy(kb_settings, &req[HEADER_LEN], SETTINGS_LEN);
		uint8_t m[HEADER_LEN + SETTINGS_LEN + 1];
		m[0]=0xf0;m[1]=0x47;m[2]=0;m[3]=0x7c;m[4]='v';m[5]=0;m[6]=SETTINGS_LEN+1;m[7]=0;
		memcpy(&m[HEADER_LEN], kb_settings, SETTINGS_LEN);
		m[sizeof(m)-1]=0xf7;
		size_t n = sizeof(m) < cap ? sizeof(m) : cap;
		memcpy(reply, m, n); return n;
	}
	if (cmd == 'd' && expect == 'd') {
		uint8_t m[9] = {0xf0,0x47,0,0x7c,'d',0,1,2,0xf7};
		size_t n = sizeof(m) < cap ? sizeof(m) : cap;
		memcpy(reply, m, n); return n;
	}
	return 0;
}
bool sysex_bridge_keyboard_seen(void) { return kb_online; }
void sysex_bridge_init(void) {}

static void kb_apply_write(const uint8_t *m, size_t n)
{
	if (!kb_online || n != PROGRAM_MSG) return;
	if (m[0] != 0xf0 || m[1] != 0x47 || m[3] != 0x7c || m[4] != 'a') return;
	if (m[PROGRAM_MSG - 1] != 0xf7 || m[7] >= 5) return;
	memcpy(kb_wire[m[7]], &m[HEADER_LEN], RECORD_SIZE);
}

/* ---- httpd stub implementations ---- */
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s)
{ snprintf(r->status, sizeof(r->status), "%s", s); return 0; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{ snprintf(r->resp, sizeof(r->resp), "%s", s); r->resp_len = strlen(s); r->sent = 1; return 0; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long len)
{ if (len > (long)sizeof(r->resp)-1) len = sizeof(r->resp)-1;
  memcpy(r->resp, buf, len); r->resp[len]=0; r->resp_len=len; r->sent=1; return 0; }
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{ size_t n = strlen(r->test_body); if (n > len) n = len; memcpy(buf, r->test_body, n); return (int)n; }

/* ---- harness ---- */
static int fails;
static void ok(const char *name, int cond, const char *detail)
{
	printf("%s %-38s %s\n", cond ? "ok  " : "FAIL", name, detail ? detail : "");
	if (!cond) fails++;
}

static const char *json_find(const char *body, const char *key)
{
	static char out[64];
	char pat[40]; snprintf(pat, sizeof pat, "\"%s\":", key);
	const char *at = strstr(body, pat);
	if (!at) return NULL;
	at += strlen(pat);
	size_t i = 0;
	while (*at && *at != ',' && *at != '}' && i < sizeof(out)-1) out[i++] = *at++;
	out[i] = 0;
	return out;
}

int main(void)
{
	char buf[160];

	{
		httpd_req_t req = {0};
		req.uri = "/generate_204";
		captive_redirect_get(&req);
		ok("captive probe redirects", !strcmp(req.status, "303 See Other") &&
		   strstr(req.resp, "MPK mini editor") != NULL, req.status);
	}

	/* ---- 1. the duplicated table matches the firmware's ---- */
	{
		FILE *f = fopen(FIRMWARE_PROGRAM_C, "r");
		ok("firmware source is readable", f != NULL, FIRMWARE_PROGRAM_C);
		if (!f) return 1;
		static char src[200000];
		size_t n = fread(src, 1, sizeof(src)-1, f); src[n] = 0; fclose(f);
		const char *at = strstr(src, "RECORD_TO_WIRE[PROGRAM_RECORD_SIZE] = {");
		ok("found RECORD_TO_WIRE in firmware", at != NULL, NULL);
		if (!at) return 1;
		at = strchr(at, '{') + 1;
		int match = 1, count = 0;
		for (int i = 0; i < RECORD_SIZE; i++) {
			long v = strtol(at, (char **)&at, 10);
			if (v != RECORD_TO_WIRE[i]) match = 0;
			count++;
			at = strchr(at, ',') ? strchr(at, ',') + 1 : at;
		}
		sprintf(buf, "%d entries compared", count);
		ok("wire table matches the firmware's", match && count == RECORD_SIZE, buf);
	}

	/* ---- 2. the table is a true permutation ---- */
	{
		int seen[RECORD_SIZE]; memset(seen, 0, sizeof seen);
		int perm = 1;
		for (int i = 0; i < RECORD_SIZE; i++) {
			if (RECORD_TO_WIRE[i] >= RECORD_SIZE || seen[RECORD_TO_WIRE[i]]) perm = 0;
			else seen[RECORD_TO_WIRE[i]] = 1;
		}
		ok("wire table is a permutation", perm, "no collisions, nothing dropped");
	}

	/* ---- 3. rec_set / rec_get round trip every offset ---- */
	{
		uint8_t wire[RECORD_SIZE]; memset(wire, 0, sizeof wire);
		int round = 1;
		for (int i = 0; i < RECORD_SIZE; i++) rec_set(wire, (uint8_t)i, (uint8_t)(i + 1));
		for (int i = 0; i < RECORD_SIZE; i++)
			if (rec_get(wire, (uint8_t)i) != (uint8_t)((i + 1) & 0x7f)) round = 0;
		ok("record offsets round trip", round, "all 101 offsets");
	}

	/* ---- 4. GET /api/program/2 decodes the real layout ---- */
	{
		/* Build a record with distinctive values, then push it through
		 * the same wire order the keyboard would use. */
		uint8_t wire[RECORD_SIZE]; memset(wire, 0, sizeof wire);
		rec_set(wire, R_CHANNEL, 9);
		rec_set(wire, R_PAD_CHANNEL, 11);
		rec_set(wire, R_OCTAVE, 6);
		rec_set(wire, R_FINE, 14);
		rec_set(wire, R_ARP_MODE, 4);
		rec_set(wire, R_TEMPO_LOW, 1); rec_set(wire, R_TEMPO_HIGH, 12); /* 140 */
		rec_set(wire, R_PAD_BASE + 3 * PAD_STRIDE + 0, 61);   /* pad 4 note A */
		rec_set(wire, R_PAD_BASE + 3 * PAD_STRIDE + 6, 1);    /* pad 4 toggle */
		rec_set(wire, R_KNOB_BASE + 5 * 3 + 0, 74);           /* knob 6 CC */
		rec_set(wire, R_KNOB_BASE + 5 * 3 + 2, 120);          /* knob 6 high */
		memcpy(kb_wire[2], wire, RECORD_SIZE);

		httpd_req_t req = {0}; req.uri = "/api/program/2";
		program_get(&req);
		ok("program GET answers", req.sent && strstr(req.resp, "\"pads\"") != NULL, NULL);
		ok("channel decoded",  !strcmp(json_find(req.resp, "channel"), "9"), "");
		ok("pad channel decoded", !strcmp(json_find(req.resp, "padChannel"), "11"), NULL);
		ok("octave decoded",   !strcmp(json_find(req.resp, "octave"), "6"), NULL);
		ok("arp mode decoded", !strcmp(json_find(req.resp, "arpMode"), "4"), NULL);
		ok("tempo recombined", !strcmp(json_find(req.resp, "tempo"), "140"), "1*128+12");
		ok("pad note decoded", strstr(req.resp, "\"noteA\":61") != NULL, NULL);
		ok("knob cc decoded",  strstr(req.resp, "\"cc\":74") != NULL, NULL);
	}

	/* ---- 5. POST is read-modify-write: untouched fields survive ---- */
	{
		memset(kb_wire[1], 0, RECORD_SIZE);
		rec_set(kb_wire[1], R_CHANNEL, 3);
		rec_set(kb_wire[1], R_KNOB_BASE, 99);                 /* knob 1 CC */
		rec_set(kb_wire[1], R_PAD_BASE + 7 * PAD_STRIDE, 55); /* pad 8 note A */

		uart_tx_len = 0;
		httpd_req_t req = {0};
		req.uri = "/api/program/1";
		req.test_body = "{\"octave\":7}";
		req.content_len = strlen(req.test_body);
		program_post(&req);

		ok("POST accepted", req.sent && strstr(req.resp, "error") == NULL, req.resp);
		ok("edited field applied", rec_get(kb_wire[1], R_OCTAVE) == 7, NULL);
		ok("untouched channel survived", rec_get(kb_wire[1], R_CHANNEL) == 3, NULL);
		ok("untouched knob survived", rec_get(kb_wire[1], R_KNOB_BASE) == 99, NULL);
		ok("untouched pad survived",
		   rec_get(kb_wire[1], R_PAD_BASE + 7 * PAD_STRIDE) == 55, NULL);
	}

	/* ---- 6. tempo is split and clamped ---- */
	{
		memset(kb_wire[1], 0, RECORD_SIZE);
		uart_tx_len = 0;
		httpd_req_t req = {0};
		req.uri = "/api/program/1";
		req.test_body = "{\"tempo\":300}";
		req.content_len = strlen(req.test_body);
		program_post(&req);
		unsigned t = rec_get(kb_wire[1], R_TEMPO_LOW) * 128u + rec_get(kb_wire[1], R_TEMPO_HIGH);
		sprintf(buf, "300 clamped to %u", t);
		ok("tempo clamps to 240", t == 240, buf);
	}

	/* ---- 7. every byte written to the keyboard is 7-bit safe ---- */
	{
		memset(kb_wire[1], 0, RECORD_SIZE);
		uart_tx_len = 0;
		httpd_req_t req = {0};
		req.uri = "/api/program/1";
		req.test_body = "{\"channel\":200,\"p0noteA\":250,\"k0cc\":199}";
		req.content_len = strlen(req.test_body);
		program_post(&req);
		int safe = 1;
		for (size_t i = 0; i < uart_tx_len; i++) {
			/* status bytes F0/F7 aside, SysEx data must have bit 7 clear */
			if (uart_tx[i] >= 0x80 && uart_tx[i] != 0xf0 && uart_tx[i] != 0xf7) safe = 0;
		}
		sprintf(buf, "%zu bytes checked", uart_tx_len);
		ok("all SysEx data bytes are 7-bit", safe, buf);
	}

	/* ---- 8. bad program index is rejected, not clamped ---- */
	{
		httpd_req_t req = {0}; req.uri = "/api/program/9";
		program_get(&req);
		ok("program 9 rejected", strstr(req.resp, "error") != NULL &&
		   strstr(req.status, "400") != NULL, req.status);
	}

	/* ---- 9. an unresponsive keyboard surfaces as an error ---- */
	{
		kb_online = false;
		httpd_req_t req = {0}; req.uri = "/api/program/2";
		program_get(&req);
		ok("timeout reported, not faked", strstr(req.resp, "error") != NULL &&
		   strstr(req.status, "504") != NULL, req.status);
		kb_online = true;
	}

	/* ---- 10. settings round trip ---- */
	{
		httpd_req_t req = {0};
		req.test_body = "{\"keyCurve\":3,\"padCurve\":1,\"keyFixed\":77,\"padFixed\":90}";
		req.content_len = strlen(req.test_body);
		settings_post(&req);
		ok("settings write accepted", strstr(req.resp, "error") == NULL, req.resp);
		ok("key curve stored", kb_settings[0] == 3, NULL);
		ok("pad curve stored", kb_settings[1] == 1, NULL);
		ok("key fixed stored", kb_settings[2] == 77, NULL);
		ok("settings echoed back", strstr(req.resp, "\"keyCurve\":3") != NULL, NULL);
	}

	/* ---- 11. partial settings edit keeps the rest ---- */
	{
		kb_settings[0] = 2; kb_settings[1] = 4; kb_settings[2] = 60; kb_settings[3] = 61;
		httpd_req_t req = {0};
		req.test_body = "{\"padCurve\":5}";
		req.content_len = strlen(req.test_body);
		settings_post(&req);
		ok("only the named setting changed",
		   kb_settings[0] == 2 && kb_settings[1] == 5 &&
		   kb_settings[2] == 60 && kb_settings[3] == 61, NULL);
	}

	/* ---- 12. json_uint rejects junk rather than guessing ---- */
	{
		unsigned v = 12345;
		ok("missing key reports absent", !json_uint("{\"a\":1}", "b", &v), NULL);
		ok("non numeric reports absent", !json_uint("{\"a\":\"x\"}", "a", &v), NULL);
		ok("value parsed", json_uint("{\"a\": 42}", "a", &v) && v == 42, NULL);
		ok("key is not matched as substring",
		   !json_uint("{\"padCurve\":3}", "Curve", &v), "'Curve' must not hit 'padCurve'");
	}

	/* ---- 13. JSON buffer overflow is reported, never truncated silently ---- */
	{
		uint8_t wire[RECORD_SIZE]; memset(wire, 127, sizeof wire);
		char small[80];
		ok("undersized buffer reports failure",
		   program_to_json(wire, small, sizeof small) < 0, "returns < 0");
		char big[1400];
		int n = program_to_json(wire, big, sizeof big);
		sprintf(buf, "%d bytes at max values", n);
		ok("worst case JSON fits the real buffer", n > 0, buf);
	}

	printf("\n%s\n", fails ? "FAILURES PRESENT" : "all editor tests passed");
	return fails != 0;
}
