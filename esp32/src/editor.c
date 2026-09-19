/*
 * WiFi editor portal: SoftAP + HTTP server + the SysEx translation that
 * turns the browser's JSON into the keyboard's own editor protocol.
 *
 * The 101-byte program payload travels in the original firmware's "wire"
 * order, which is a pure byte reorder of its in-memory record (see
 * firmware/src/program.c). RECORD_TO_WIRE below mirrors that table so
 * this side can address fields by their record offset and hand the
 * browser named JSON instead of an opaque blob. It is a fixed property
 * of the original protocol, not a value that drifts with our firmware.
 */
#include "editor.h"
#include "captive_dns.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sysex_bridge.h"

static const char *TAG = "editor";

#define AP_SSID     "MPK-mini-Open"
#define AP_PASSWORD "mpkmini1"        /* WPA2 needs 8 characters minimum */
#define AP_CHANNEL  6
#define AP_MAX_CONN 2
#define PORTAL_URL  "http://192.168.4.1/"

/*
 * Transmit power cap, in units of 0.25 dBm -- 44 is 11 dBm.
 *
 * At full power an ESP32-C3 draws 276 mA or more while transmitting, and
 * some SuperMini batches regulate with an LP5907 (SMD marking LLVB) rated
 * for only 250 mA. The portal serves one browser, usually in the same
 * room as the instrument, so the range full power buys is worthless here
 * while the current spike is not. Capping to 11 dBm keeps the peak inside
 * the smaller regulator's rating and costs nothing on boards carrying the
 * 500 mA part.
 */
#define AP_TX_POWER 44

#define RECORD_SIZE   101
#define HEADER_LEN    8
#define PROGRAM_MSG   110             /* header + 101 payload + F7 */
#define SETTINGS_LEN  6
#define SYSEX_TIMEOUT 800

/* Mirrors firmware/src/program.c's RECORD_TO_WIRE. */
static const uint8_t RECORD_TO_WIRE[RECORD_SIZE] = {
	1,0,2,3,4,5,6,7,8,9,10,11,12,13,45,14,46,15,47,16,48,17,49,18,50,19,51,
	20,52,21,53,22,54,23,55,24,56,25,57,26,58,27,59,28,60,29,61,30,62,31,63,
	32,64,33,65,34,66,35,67,36,68,37,69,38,70,39,71,40,72,41,73,42,74,43,75,
	44,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,
	99,100
};

/* Record offsets, from firmware/include/program.h. */
#define R_CHANNEL 0x00
#define R_PAD_CHANNEL 0x01
#define R_OCTAVE 0x02
#define R_FINE 0x03
#define R_ARP_ENABLED 0x04
#define R_ARP_MODE 0x05
#define R_ARP_DIV 0x06
#define R_ARP_EXTERNAL 0x07
#define R_ARP_LATCH 0x08
#define R_TAP_COUNT 0x09
#define R_TEMPO_LOW 0x0a
#define R_TEMPO_HIGH 0x0b
#define R_ARP_RANGE 0x0c
#define R_PAD_BASE 0x0d
#define PAD_STRIDE 8
#define R_KNOB_BASE 0x4d

static httpd_handle_t server;
static esp_netif_t *ap_netif;
static bool portal_active;
static bool netif_ready;
static captive_dns_handle_t dns_server;

#ifdef EDITOR_HOST_TEST
extern const uint8_t editor_html_start[];
extern const uint8_t editor_html_end[];
#else
extern const uint8_t editor_html_start[] asm("_binary_editor_html_start");
extern const uint8_t editor_html_end[]   asm("_binary_editor_html_end");
#endif

/* ---------- record access ---------- */

static uint8_t rec_get(const uint8_t *wire, uint8_t record_offset)
{
	return wire[RECORD_TO_WIRE[record_offset]];
}

static void rec_set(uint8_t *wire, uint8_t record_offset, uint8_t value)
{
	wire[RECORD_TO_WIRE[record_offset]] = value & 0x7fu;
}

/* ---------- SysEx helpers ---------- */

static bool fetch_program(uint8_t index, uint8_t wire[RECORD_SIZE])
{
	const uint8_t request[9] = {0xf0, 0x47, 0x00, 0x7c, 'c', 0x00, 0x01, index, 0xf7};
	uint8_t reply[SYSEX_BRIDGE_MAX];
	size_t len = sysex_bridge_request(request, sizeof(request), 'c',
	                                  reply, sizeof(reply), SYSEX_TIMEOUT);
	if (len != PROGRAM_MSG) return false;
	memcpy(wire, &reply[HEADER_LEN], RECORD_SIZE);
	return true;
}

static bool store_program(uint8_t index, const uint8_t wire[RECORD_SIZE])
{
	uint8_t message[PROGRAM_MSG];
	message[0] = 0xf0; message[1] = 0x47; message[2] = 0x00; message[3] = 0x7c;
	message[4] = 'a';  message[5] = 0x00; message[6] = 0x66; message[7] = index;
	memcpy(&message[HEADER_LEN], wire, RECORD_SIZE);
	message[PROGRAM_MSG - 1] = 0xf7;

	/* 'a' has no reply of its own, so read the program back to confirm
	 * the keyboard actually took it rather than reporting a blind success. */
	extern void midi_uart_send(const uint8_t *bytes, size_t len);
	midi_uart_send(message, sizeof(message));
	vTaskDelay(pdMS_TO_TICKS(60)); /* a write to program 1-4 erases a flash page */

	uint8_t readback[RECORD_SIZE];
	if (!fetch_program(index, readback)) return false;
	return memcmp(readback, wire, RECORD_SIZE) == 0;
}

static bool fetch_settings(uint8_t out[SETTINGS_LEN])
{
	const uint8_t request[9] = {0xf0, 0x47, 0x00, 0x7c, 'v', 0x00, 0x01, 0x00, 0xf7};
	uint8_t reply[SYSEX_BRIDGE_MAX];
	size_t len = sysex_bridge_request(request, sizeof(request), 'v',
	                                  reply, sizeof(reply), SYSEX_TIMEOUT);
	if (len != HEADER_LEN + SETTINGS_LEN + 1) return false;
	memcpy(out, &reply[HEADER_LEN], SETTINGS_LEN);
	return true;
}

static bool store_settings(const uint8_t in[SETTINGS_LEN])
{
	uint8_t message[HEADER_LEN + SETTINGS_LEN + 1];
	message[0] = 0xf0; message[1] = 0x47; message[2] = 0x00; message[3] = 0x7c;
	message[4] = 'v';  message[5] = 0x00; message[6] = SETTINGS_LEN + 1;
	message[7] = 0x01; /* write */
	for (int i = 0; i < SETTINGS_LEN; i++) message[HEADER_LEN + i] = in[i] & 0x7fu;
	message[sizeof(message) - 1] = 0xf7;

	uint8_t reply[SYSEX_BRIDGE_MAX];
	/* The keyboard echoes the stored values back, so the reply doubles
	 * as confirmation that the write landed and was in range. */
	size_t len = sysex_bridge_request(message, sizeof(message), 'v',
	                                  reply, sizeof(reply), SYSEX_TIMEOUT + 400);
	return len == HEADER_LEN + SETTINGS_LEN + 1;
}

/* ---------- JSON ---------- */

static int append(char *buf, int cap, int pos, const char *fmt, ...)
{
	if (pos < 0 || pos >= cap) return -1;
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(buf + pos, (size_t)(cap - pos), fmt, args);
	va_end(args);
	if (n < 0 || n >= cap - pos) return -1;
	return pos + n;
}

static int program_to_json(const uint8_t *wire, char *buf, int cap)
{
	int p = append(buf, cap, 0,
		"{\"channel\":%u,\"padChannel\":%u,\"octave\":%u,\"transpose\":%u,"
		"\"arpOn\":%u,\"arpMode\":%u,\"arpDiv\":%u,\"arpExternal\":%u,"
		"\"arpLatch\":%u,\"arpRange\":%u,\"tapCount\":%u,\"tempo\":%u,",
		rec_get(wire, R_CHANNEL), rec_get(wire, R_PAD_CHANNEL),
		rec_get(wire, R_OCTAVE), rec_get(wire, R_FINE),
		rec_get(wire, R_ARP_ENABLED), rec_get(wire, R_ARP_MODE),
		rec_get(wire, R_ARP_DIV), rec_get(wire, R_ARP_EXTERNAL),
		rec_get(wire, R_ARP_LATCH), rec_get(wire, R_ARP_RANGE),
		rec_get(wire, R_TAP_COUNT),
		(unsigned)rec_get(wire, R_TEMPO_LOW) * 128u + rec_get(wire, R_TEMPO_HIGH));

	p = append(buf, cap, p, "\"pads\":[");
	for (int i = 0; i < 8; i++) {
		uint8_t b = (uint8_t)(R_PAD_BASE + i * PAD_STRIDE);
		p = append(buf, cap, p,
			"%s{\"noteA\":%u,\"noteB\":%u,\"pcA\":%u,\"pcB\":%u,"
			"\"ccA\":%u,\"ccB\":%u,\"toggle\":%u}",
			i ? "," : "",
			rec_get(wire, b + 0), rec_get(wire, b + 1),
			rec_get(wire, b + 2), rec_get(wire, b + 3),
			rec_get(wire, b + 4), rec_get(wire, b + 5),
			rec_get(wire, b + 6));
	}

	p = append(buf, cap, p, "],\"knobs\":[");
	for (int i = 0; i < 8; i++) {
		uint8_t b = (uint8_t)(R_KNOB_BASE + i * 3);
		p = append(buf, cap, p, "%s{\"cc\":%u,\"low\":%u,\"high\":%u}",
			i ? "," : "",
			rec_get(wire, b + 0), rec_get(wire, b + 1), rec_get(wire, b + 2));
	}
	return append(buf, cap, p, "]}");
}

/* Minimal extractor for the flat JSON the editor page sends. Values are
 * unsigned integers only; anything missing keeps the value already in the
 * record, so a partial edit never blanks the rest of the program. */
static bool json_uint(const char *body, const char *key, unsigned *out)
{
	char pattern[32];
	int n = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	if (n <= 0 || n >= (int)sizeof(pattern)) return false;
	const char *at = strstr(body, pattern);
	if (at == NULL) return false;
	at += n;
	while (*at == ' ') at++;
	if (*at < '0' || *at > '9') return false;
	*out = (unsigned)strtoul(at, NULL, 10);
	return true;
}

static void json_field_to_record(const char *body, const char *key,
                                 uint8_t *wire, uint8_t offset)
{
	unsigned v;
	if (json_uint(body, key, &v)) rec_set(wire, offset, (uint8_t)v);
}

/* ---------- HTTP handlers ---------- */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
	char body[160];
	snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
	httpd_resp_set_status(req, status);
	return send_json(req, body);
}

static esp_err_t root_get(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	return httpd_resp_send(req, (const char *)editor_html_start,
	                       editor_html_end - editor_html_start - 1);
}

/* Connectivity probes use several vendor-specific paths. Any unknown GET is
 * sent to the editor; a small body is important for iOS portal detection. */
static esp_err_t captive_redirect_get(httpd_req_t *req)
{
	httpd_resp_set_status(req, "303 See Other");
	httpd_resp_set_hdr(req, "Location", PORTAL_URL);
	httpd_resp_set_type(req, "text/plain");
	return httpd_resp_sendstr(req, "Open the MPK mini editor");
}

static esp_err_t status_get(httpd_req_t *req)
{
	const uint8_t request[9] = {0xf0, 0x47, 0x00, 0x7c, 'd', 0x00, 0x01, 0x00, 0xf7};
	uint8_t reply[SYSEX_BRIDGE_MAX];
	size_t len = sysex_bridge_request(request, sizeof(request), 'd',
	                                  reply, sizeof(reply), SYSEX_TIMEOUT);
	char body[128];
	if (len >= 9) {
		snprintf(body, sizeof(body),
		         "{\"keyboard\":true,\"currentProgram\":%u}", reply[7]);
	} else {
		snprintf(body, sizeof(body), "{\"keyboard\":false,\"currentProgram\":null}");
	}
	return send_json(req, body);
}

static bool program_index_from_uri(httpd_req_t *req, uint8_t *index)
{
	const char *last = strrchr(req->uri, '/');
	if (last == NULL || last[1] == '\0') return false;
	unsigned long v = strtoul(last + 1, NULL, 10);
	if (v > 4) return false;
	*index = (uint8_t)v;
	return true;
}

static esp_err_t program_get(httpd_req_t *req)
{
	uint8_t index;
	if (!program_index_from_uri(req, &index))
		return send_error(req, "400 Bad Request", "program must be 0-4");

	uint8_t wire[RECORD_SIZE];
	if (!fetch_program(index, wire))
		return send_error(req, "504 Gateway Timeout", "keyboard did not answer");

	char body[1400];
	if (program_to_json(wire, body, sizeof(body)) < 0)
		return send_error(req, "500 Internal Server Error", "response too large");
	return send_json(req, body);
}

static esp_err_t program_post(httpd_req_t *req)
{
	uint8_t index;
	if (!program_index_from_uri(req, &index))
		return send_error(req, "400 Bad Request", "program must be 0-4");
	if (req->content_len >= 1400)
		return send_error(req, "413 Payload Too Large", "body too large");

	char body[1400];
	int received = httpd_req_recv(req, body, req->content_len);
	if (received <= 0) return send_error(req, "400 Bad Request", "could not read body");
	body[received] = '\0';

	/* Read-modify-write: the page may send only the fields it changed,
	 * and everything else must survive untouched. */
	uint8_t wire[RECORD_SIZE];
	if (!fetch_program(index, wire))
		return send_error(req, "504 Gateway Timeout", "keyboard did not answer");

	json_field_to_record(body, "channel", wire, R_CHANNEL);
	json_field_to_record(body, "padChannel", wire, R_PAD_CHANNEL);
	json_field_to_record(body, "octave", wire, R_OCTAVE);
	json_field_to_record(body, "transpose", wire, R_FINE);
	json_field_to_record(body, "arpOn", wire, R_ARP_ENABLED);
	json_field_to_record(body, "arpMode", wire, R_ARP_MODE);
	json_field_to_record(body, "arpDiv", wire, R_ARP_DIV);
	json_field_to_record(body, "arpExternal", wire, R_ARP_EXTERNAL);
	json_field_to_record(body, "arpLatch", wire, R_ARP_LATCH);
	json_field_to_record(body, "arpRange", wire, R_ARP_RANGE);
	json_field_to_record(body, "tapCount", wire, R_TAP_COUNT);

	unsigned tempo;
	if (json_uint(body, "tempo", &tempo)) {
		if (tempo < 30) tempo = 30;
		if (tempo > 240) tempo = 240;
		rec_set(wire, R_TEMPO_LOW, (uint8_t)(tempo / 128u));
		rec_set(wire, R_TEMPO_HIGH, (uint8_t)(tempo % 128u));
	}

	static const char *pad_keys[7] = {"noteA", "noteB", "pcA", "pcB", "ccA", "ccB", "toggle"};
	for (int i = 0; i < 8; i++) {
		for (int f = 0; f < 7; f++) {
			char key[16];
			snprintf(key, sizeof(key), "p%d%s", i, pad_keys[f]);
			json_field_to_record(body, key, wire,
			                     (uint8_t)(R_PAD_BASE + i * PAD_STRIDE + f));
		}
	}
	static const char *knob_keys[3] = {"cc", "low", "high"};
	for (int i = 0; i < 8; i++) {
		for (int f = 0; f < 3; f++) {
			char key[16];
			snprintf(key, sizeof(key), "k%d%s", i, knob_keys[f]);
			json_field_to_record(body, key, wire, (uint8_t)(R_KNOB_BASE + i * 3 + f));
		}
	}

	if (!store_program(index, wire))
		return send_error(req, "502 Bad Gateway", "keyboard did not confirm the write");

	char out[1400];
	if (program_to_json(wire, out, sizeof(out)) < 0)
		return send_error(req, "500 Internal Server Error", "response too large");
	return send_json(req, out);
}

static esp_err_t settings_get(httpd_req_t *req)
{
	uint8_t s[SETTINGS_LEN];
	if (!fetch_settings(s))
		return send_error(req, "504 Gateway Timeout", "keyboard did not answer");
	char body[224];
	snprintf(body, sizeof(body),
	         "{\"keyCurve\":%u,\"padCurve\":%u,\"keyFixed\":%u,\"padFixed\":%u,"
	         "\"keyFastMs\":%u,\"keySlowMs\":%u}",
	         s[0], s[1], s[2], s[3], s[4], s[5]);
	return send_json(req, body);
}

static esp_err_t settings_post(httpd_req_t *req)
{
	if (req->content_len >= 256)
		return send_error(req, "413 Payload Too Large", "body too large");
	char body[256];
	int received = httpd_req_recv(req, body, req->content_len);
	if (received <= 0) return send_error(req, "400 Bad Request", "could not read body");
	body[received] = '\0';

	uint8_t s[SETTINGS_LEN];
	if (!fetch_settings(s))
		return send_error(req, "504 Gateway Timeout", "keyboard did not answer");

	static const char *keys[SETTINGS_LEN] = {
		"keyCurve", "padCurve", "keyFixed", "padFixed", "keyFastMs", "keySlowMs"
	};
	for (int i = 0; i < SETTINGS_LEN; i++) {
		unsigned v;
		if (json_uint(body, keys[i], &v)) s[i] = (uint8_t)(v & 0x7fu);
	}
	if (!store_settings(s))
		return send_error(req, "502 Bad Gateway", "keyboard did not confirm the write");
	return settings_get(req);
}

/* Calibration telemetry: the shortest, longest and most recent contact
 * intervals the keybed has produced since boot. What the velocity window
 * should be set from. */
static esp_err_t velstats_get(httpd_req_t *req)
{
	const uint8_t request[9] = {0xf0, 0x47, 0x00, 0x7c, 'v', 0x00, 0x01, 0x02, 0xf7};
	uint8_t reply[SYSEX_BRIDGE_MAX];
	size_t len = sysex_bridge_request(request, sizeof(request), 'v',
	                                  reply, sizeof(reply), SYSEX_TIMEOUT);
	if (len != HEADER_LEN + 4 + 1)
		return send_error(req, "504 Gateway Timeout", "keyboard did not answer");
	char body[160];
	snprintf(body, sizeof(body),
	         "{\"fastestMs\":%u,\"slowestMs\":%u,\"lastMs\":%u,\"strikes\":%u}",
	         reply[HEADER_LEN], reply[HEADER_LEN + 1],
	         reply[HEADER_LEN + 2], reply[HEADER_LEN + 3]);
	return send_json(req, body);
}

/* ---------- lifecycle ---------- */

static const httpd_uri_t routes[] = {
	{.uri = "/",              .method = HTTP_GET,  .handler = root_get},
	{.uri = "/api/status",    .method = HTTP_GET,  .handler = status_get},
	{.uri = "/api/settings",  .method = HTTP_GET,  .handler = settings_get},
	{.uri = "/api/settings",  .method = HTTP_POST, .handler = settings_post},
	{.uri = "/api/velstats",  .method = HTTP_GET,  .handler = velstats_get},
	{.uri = "/api/program/*", .method = HTTP_GET,  .handler = program_get},
	{.uri = "/api/program/*", .method = HTTP_POST, .handler = program_post},
	{.uri = "/*",             .method = HTTP_GET,  .handler = captive_redirect_get},
};

static void configure_captive_portal(void)
{
	/* Option 114 is the standards-based captive-portal hint. Offering this AP
	 * as DNS as well supports older Android, Apple, and Windows probes. */
	esp_err_t err = esp_netif_dhcps_stop(ap_netif);
	if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
		ESP_LOGW(TAG, "could not stop DHCP server: %d", err);

	uint8_t offer_dns = 1;
	err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
	                             ESP_NETIF_DOMAIN_NAME_SERVER,
	                             &offer_dns, sizeof(offer_dns));
	if (err != ESP_OK) ESP_LOGW(TAG, "could not advertise portal DNS: %d", err);

	static char portal_url[] = PORTAL_URL;
	err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
	                             ESP_NETIF_CAPTIVEPORTAL_URI,
	                             portal_url, strlen(portal_url));
	if (err != ESP_OK) ESP_LOGW(TAG, "could not advertise portal URL: %d", err);

	err = esp_netif_dhcps_start(ap_netif);
	if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
		ESP_LOGW(TAG, "could not restart DHCP server: %d", err);
}

static void portal_start(void)
{
	if (!netif_ready) {
		ESP_ERROR_CHECK(esp_netif_init());
		ESP_ERROR_CHECK(esp_event_loop_create_default());
		ap_netif = esp_netif_create_default_wifi_ap();
		wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
		ESP_ERROR_CHECK(esp_wifi_init(&cfg));
		netif_ready = true;
	}

	wifi_config_t wifi = {0};
	memcpy(wifi.ap.ssid, AP_SSID, sizeof(AP_SSID));
	wifi.ap.ssid_len = strlen(AP_SSID);
	memcpy(wifi.ap.password, AP_PASSWORD, sizeof(AP_PASSWORD));
	wifi.ap.channel = AP_CHANNEL;
	wifi.ap.max_connection = AP_MAX_CONN;
	wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
	ESP_ERROR_CHECK(esp_wifi_start());
	configure_captive_portal();
	/* Both of these must follow esp_wifi_start(). BLE MIDI keeps running
	 * while the portal is up, so leave the radio to the coexistence
	 * scheduler rather than chasing WiFi throughput. */
	esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
	esp_err_t power = esp_wifi_set_max_tx_power(AP_TX_POWER);
	if (power != ESP_OK) ESP_LOGW(TAG, "could not cap TX power: %d", power);

	int8_t actual = 0;
	if (esp_wifi_get_max_tx_power(&actual) == ESP_OK)
		ESP_LOGI(TAG, "WiFi TX power %.2f dBm", actual / 4.0);

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.uri_match_fn = httpd_uri_match_wildcard;
	config.max_uri_handlers = 10;
	config.stack_size = 5120;
	config.lru_purge_enable = true;
	/* httpd needs max_open_sockets + 3 internal sockets to fit inside
	 * CONFIG_LWIP_MAX_SOCKETS, and the default asks for more than this
	 * build allows -- it refuses to start otherwise. One browser opening
	 * a handful of parallel requests is all this portal serves, and
	 * lru_purge_enable recycles the oldest when they run out. */
	config.max_open_sockets = 4;
	if (httpd_start(&server, &config) != ESP_OK) {
		ESP_LOGE(TAG, "HTTP server failed to start");
		esp_wifi_stop();
		return;
	}
	for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
		httpd_register_uri_handler(server, &routes[i]);

	esp_netif_ip_info_t ip_info;
	if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK)
		dns_server = captive_dns_start(ip_info.ip.addr);
	if (dns_server == NULL)
		ESP_LOGW(TAG, "wildcard DNS failed to start; DHCP portal hint remains active");

	portal_active = true;
	ESP_LOGI(TAG, "editor at %s  ssid=%s pass=%s", PORTAL_URL, AP_SSID, AP_PASSWORD);
}

static void portal_stop(void)
{
	if (dns_server != NULL) {
		captive_dns_stop(dns_server);
		dns_server = NULL;
	}
	if (server != NULL) {
		httpd_stop(server);
		server = NULL;
	}
	esp_wifi_stop();
	portal_active = false;
	ESP_LOGI(TAG, "editor portal stopped");
}

void editor_init(void)
{
	sysex_bridge_init();
	portal_active = false;
	netif_ready = false;
	server = NULL;
	dns_server = NULL;
}

void editor_toggle(void)
{
	if (portal_active) portal_stop();
	else portal_start();
}

bool editor_active(void)
{
	return portal_active;
}
