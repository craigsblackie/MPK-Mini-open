/* Minimal wildcard DNS responder for captive-portal discovery. */
#include "captive_dns.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_PACKET_MAX 512
#define DNS_HEADER_LEN 12
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1

static const char *TAG = "captive-dns";

struct captive_dns {
	volatile bool running;
	uint32_t ap_addr;
	TaskHandle_t task;
	SemaphoreHandle_t stopped;
};

static uint16_t read_u16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void write_u16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static void write_u32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

/* Return the first byte after an uncompressed DNS name, or NULL for a
 * malformed/compressed question. Client queries normally use plain labels. */
static const uint8_t *question_name_end(const uint8_t *p, const uint8_t *end)
{
	while (p < end) {
		uint8_t label_len = *p++;
		if (label_len == 0) return p;
		if ((label_len & 0xc0u) != 0u || label_len > 63u ||
		    (size_t)(end - p) < label_len)
			return NULL;
		p += label_len;
	}
	return NULL;
}

static int make_reply(const uint8_t *request, size_t request_len,
	                  uint8_t reply[DNS_PACKET_MAX], uint32_t ap_addr)
{
	if (request_len < DNS_HEADER_LEN || request_len > DNS_PACKET_MAX)
		return -1;

	uint16_t flags = read_u16(&request[2]);
	uint16_t questions = read_u16(&request[4]);
	if ((flags & 0x8000u) != 0u || (flags & 0x7800u) != 0u ||
	    questions == 0u || questions > 8u)
		return -1;

	const uint8_t *cursor = request + DNS_HEADER_LEN;
	const uint8_t *end = request + request_len;
	uint16_t answer_count = 0;
	uint16_t offsets[8];
	uint16_t types[8];
	uint16_t classes[8];

	for (uint16_t i = 0; i < questions; i++) {
		offsets[i] = (uint16_t)(cursor - request);
		const uint8_t *name_end = question_name_end(cursor, end);
		if (name_end == NULL || (size_t)(end - name_end) < 4u) return -1;
		types[i] = read_u16(name_end);
		classes[i] = read_u16(name_end + 2);
		if (types[i] == DNS_TYPE_A && classes[i] == DNS_CLASS_IN)
			answer_count++;
		cursor = name_end + 4;
	}

	size_t question_len = (size_t)(cursor - request);
	size_t reply_len = question_len + (size_t)answer_count * 16u;
	if (reply_len > DNS_PACKET_MAX) return -1;

	memcpy(reply, request, question_len);
	/* Response + authoritative answer; retain only the request's RD bit. */
	write_u16(&reply[2], (uint16_t)(0x8400u | (flags & 0x0100u)));
	write_u16(&reply[6], answer_count);
	write_u16(&reply[8], 0);
	write_u16(&reply[10], 0); /* discard any EDNS/additional section */

	uint8_t *answer = reply + question_len;
	for (uint16_t i = 0; i < questions; i++) {
		if (types[i] != DNS_TYPE_A || classes[i] != DNS_CLASS_IN) continue;
		write_u16(answer, (uint16_t)(0xc000u | offsets[i]));
		write_u16(answer + 2, DNS_TYPE_A);
		write_u16(answer + 4, DNS_CLASS_IN);
		write_u32(answer + 6, 60); /* short TTL: the AP is intentionally transient */
		write_u16(answer + 10, 4);
		memcpy(answer + 12, &ap_addr, 4);
		answer += 16;
	}
	return (int)reply_len;
}

static void dns_task(void *param)
{
	struct captive_dns *handle = param;
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0) {
		ESP_LOGE(TAG, "socket failed: errno=%d", errno);
		goto done;
	}

	struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(DNS_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (bind(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
		ESP_LOGE(TAG, "bind failed: errno=%d", errno);
		close(sock);
		goto done;
	}

	ESP_LOGI(TAG, "wildcard DNS listening on port %d", DNS_PORT);
	while (handle->running) {
		uint8_t request[DNS_PACKET_MAX];
		struct sockaddr_storage source;
		socklen_t source_len = sizeof(source);
		int received = recvfrom(sock, request, sizeof(request), 0,
		                        (struct sockaddr *)&source, &source_len);
		if (received < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
			ESP_LOGW(TAG, "receive failed: errno=%d", errno);
			break;
		}

		uint8_t reply[DNS_PACKET_MAX];
		int reply_len = make_reply(request, (size_t)received, reply,
		                           handle->ap_addr);
		if (reply_len > 0)
			sendto(sock, reply, (size_t)reply_len, 0,
			       (struct sockaddr *)&source, source_len);
	}
	close(sock);

done:
	handle->running = false;
	xSemaphoreGive(handle->stopped);
	vTaskDelete(NULL);
}

captive_dns_handle_t captive_dns_start(uint32_t ap_addr)
{
	struct captive_dns *handle = calloc(1, sizeof(*handle));
	if (handle == NULL) return NULL;
	handle->stopped = xSemaphoreCreateBinary();
	if (handle->stopped == NULL) {
		free(handle);
		return NULL;
	}
	handle->running = true;
	handle->ap_addr = ap_addr;
	if (xTaskCreate(dns_task, "captive-dns", 3072, handle, 5,
	                &handle->task) != pdPASS) {
		vSemaphoreDelete(handle->stopped);
		free(handle);
		return NULL;
	}
	return handle;
}

void captive_dns_stop(captive_dns_handle_t handle)
{
	if (handle == NULL) return;
	handle->running = false;
	/* SO_RCVTIMEO bounds this wait even when no client has queried us. */
	if (xSemaphoreTake(handle->stopped, pdMS_TO_TICKS(500)) != pdTRUE) {
		ESP_LOGW(TAG, "DNS task did not stop in time; forcing shutdown");
		vTaskDelete(handle->task);
	}
	vSemaphoreDelete(handle->stopped);
	free(handle);
}
