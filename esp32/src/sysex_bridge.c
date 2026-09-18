#include "sysex_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sysex-bridge";

static SemaphoreHandle_t request_lock;   /* one request in flight */
static SemaphoreHandle_t reply_ready;    /* signalled by the UART task */
static uint8_t reply_buf[SYSEX_BRIDGE_MAX];
static volatile size_t reply_len;
static volatile uint8_t expected_cmd;
static volatile bool waiting;
static volatile bool keyboard_seen;

void sysex_bridge_init(void)
{
	request_lock = xSemaphoreCreateMutex();
	reply_ready = xSemaphoreCreateBinary();
	waiting = false;
	reply_len = 0;
	keyboard_seen = false;
}

bool sysex_bridge_offer(const uint8_t *message, size_t len)
{
	if (!waiting || len < 5 || len > SYSEX_BRIDGE_MAX) return false;
	/* Editor traffic is always F0 47 <id> 7C <cmd> ... */
	if (message[0] != 0xf0u || message[1] != 0x47u || message[3] != 0x7cu) return false;
	if (message[4] != expected_cmd) return false;

	memcpy(reply_buf, message, len);
	reply_len = len;
	waiting = false;
	keyboard_seen = true;
	xSemaphoreGive(reply_ready);
	return true;
}

bool sysex_bridge_keyboard_seen(void)
{
	return keyboard_seen;
}

/* Provided by main.c, which owns the UART. */
void midi_uart_send(const uint8_t *bytes, size_t len);

size_t sysex_bridge_request(const uint8_t *request, size_t request_len,
                            uint8_t expect_cmd, uint8_t *reply, size_t reply_cap,
                            uint32_t timeout_ms)
{
	if (request_lock == NULL) return 0;
	if (xSemaphoreTake(request_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return 0;

	/* Drop any stale signal from a reply that arrived after a timeout. */
	xSemaphoreTake(reply_ready, 0);
	reply_len = 0;
	expected_cmd = expect_cmd;
	waiting = true;

	midi_uart_send(request, request_len);

	size_t out = 0;
	if (xSemaphoreTake(reply_ready, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
		out = reply_len < reply_cap ? reply_len : reply_cap;
		memcpy(reply, reply_buf, out);
	} else {
		waiting = false;
		ESP_LOGW(TAG, "no reply to '%c' within %ums", expect_cmd, (unsigned)timeout_ms);
	}

	xSemaphoreGive(request_lock);
	return out;
}
