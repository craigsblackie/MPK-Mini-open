/*
 * AKAI MPK mini mk1 Bluetooth LE MIDI bridge for ESP32-C3 SuperMini.
 *
 * UART1 is a bidirectional raw-MIDI link at the standard 31250 baud:
 *   STM32 PA9  (TX) -> ESP32-C3 GPIO4 (RX)
 *   STM32 PA10 (RX) <- ESP32-C3 GPIO5 (TX)
 *
 * Outbound MIDI is parsed into discrete messages, stamped on arrival, and
 * queued. A separate task packs as many queued messages as the negotiated ATT
 * MTU allows into each BLE-MIDI notification and retries when the controller
 * is out of buffers, so a fast knob sweep cannot silently drop a Note Off.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "editor.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "sysex_bridge.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* ESP-IDF's NimBLE store component provides this but does not expose the
 * declaration from its public store/config header. Official examples declare
 * it locally as well. */
void ble_store_config_init(void);

#define MIDI_UART       UART_NUM_1
#define MIDI_RX_GPIO    4
#define MIDI_TX_GPIO    5
#define MIDI_BAUD       31250
#define INVALID_HANDLE  0xffffu
#define SYSEX_CAPACITY  256u

/* Onboard LED of the HW-466AB SuperMini: GPIO8, driven low to light. Set
 * STATUS_LED_ENABLE to 0 for a board that wires it elsewhere. GPIO8 is also a
 * boot strapping pin, but it is only driven after the ROM has sampled it. */
#define STATUS_LED_ENABLE      1
#define STATUS_LED_GPIO        8
#define STATUS_LED_ACTIVE_LOW  1
#define STATUS_LED_PERIOD_MS   100

/* BOOT button, also GPIO9's strapping duty -- only read long after boot.
 * A press toggles the WiFi editor portal (see editor.c). */
#define EDITOR_BUTTON_GPIO   9
#define EDITOR_BUTTON_STABLE 3        /* x50 ms of agreement before acting */

/* Preferred ATT MTU is 256, so a notification never needs more than that. */
#define BLE_PACKET_CAPACITY 256u

/* Outbound queue. Must be a power of two. Holds framed MIDI messages; large
 * enough for a full editor SysEx dump plus live playing on top of it. */
#define OUT_RING_SIZE 2048u
#define OUT_RING_MASK (OUT_RING_SIZE - 1u)
#define OUT_FRAME_HEADER 4u

/* 2 ms per attempt: ~200 ms of controller backpressure before giving up. */
#define NOTIFY_RETRY_LIMIT    100
#define NOTIFY_RETRY_DELAY_MS 2

static const char *TAG = "mpk-ble-midi";

/* BLE_UUID128_INIT takes UUID bytes least-significant byte first. */
static const ble_uuid128_t midi_service_uuid = BLE_UUID128_INIT(
	0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7,
	0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03);
static const ble_uuid128_t midi_characteristic_uuid = BLE_UUID128_INIT(
	0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1,
	0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77);

static uint8_t own_addr_type;
static uint16_t connection_handle = INVALID_HANDLE;
static uint16_t midi_value_handle;
static bool notifications_enabled;
/* Set once the BLE host has synced and advertising has started. Used to
 * decide that a freshly installed image is healthy; see
 * confirm_running_image(). */
static volatile bool ble_synced;

typedef struct {
	uint8_t running_status;
	uint8_t message[3];
	uint8_t message_len;
	uint8_t message_needed;
	bool in_sysex;
	uint16_t sysex_len;
	uint8_t sysex[SYSEX_CAPACITY];
} midi_parser_t;

static midi_parser_t uart_parser;

static uint8_t out_ring[OUT_RING_SIZE];
static volatile uint16_t out_head;
static volatile uint16_t out_tail;
static TaskHandle_t ble_tx_task_handle;
static TaskHandle_t editor_control_task_handle;

/* Private internal-UART command emitted by the STM32 after PROGRAM has been
 * held for two seconds.  0x7D is the non-commercial manufacturer ID. */
static bool editor_control_message(const uint8_t *message, size_t len)
{
	static const uint8_t command[] = {
		0xf0, 0x7d, 'M', 'P', 'K', 0x01, 0xf7
	};
	if (len != sizeof(command) || memcmp(message, command, sizeof(command)) != 0)
		return false;
	if (editor_control_task_handle != NULL)
		xTaskNotifyGive(editor_control_task_handle);
	return true;
}

static uint8_t midi_message_size(uint8_t status)
{
	if (status < 0x80u) return 0;
	if (status < 0xc0u) return 3;
	if (status < 0xe0u) return 2;
	if (status < 0xf0u) return 3;
	switch (status) {
	case 0xf1:
	case 0xf3:
		return 2;
	case 0xf2:
		return 3;
	case 0xf6:
	case 0xf7:
	case 0xf8:
	case 0xf9:
	case 0xfa:
	case 0xfb:
	case 0xfc:
	case 0xfd:
	case 0xfe:
	case 0xff:
		return 1;
	default:
		return 0;
	}
}

static bool midi_is_realtime(uint8_t status)
{
	return status >= 0xf8u;
}

static bool midi_is_system_common(uint8_t status)
{
	return status == 0xf1u || status == 0xf2u || status == 0xf3u ||
	       status == 0xf6u;
}

/* 13-bit millisecond timestamp, as the BLE-MIDI specification defines it. */
static uint16_t midi_timestamp(void)
{
	return (uint16_t)((esp_timer_get_time() / 1000) & 0x1fffu);
}

static bool ble_midi_ready(void)
{
	return notifications_enabled && connection_handle != INVALID_HANDLE;
}

static size_t ble_value_capacity(void)
{
	if (connection_handle == INVALID_HANDLE) return 0;
	uint16_t mtu = ble_att_mtu(connection_handle);
	size_t capacity = mtu > 3u ? (size_t)mtu - 3u : 0u;
	return capacity > BLE_PACKET_CAPACITY ? BLE_PACKET_CAPACITY : capacity;
}

/*
 * Outbound queue. Single producer (the UART task) and single consumer (the BLE
 * transmit task) on a single-core part, so plain volatile indices are enough:
 * each side only advances its own. Frames are stored as
 * [len_lo][len_hi][ts_lo][ts_hi] followed by the raw MIDI message.
 */
static uint16_t out_used(void)
{
	return (uint16_t)((out_head - out_tail) & OUT_RING_MASK);
}

static uint16_t out_free(void)
{
	return (uint16_t)(OUT_RING_MASK - out_used());
}

static void out_reset(void)
{
	out_tail = out_head;
}

static void out_push(const uint8_t *message, uint16_t len)
{
	if (len == 0 || !ble_midi_ready()) return;
	if (out_free() < (uint16_t)(len + OUT_FRAME_HEADER)) {
		ESP_LOGW(TAG, "outbound queue full; dropping %u byte message", len);
		return;
	}

	uint16_t timestamp = midi_timestamp();
	const uint8_t header[OUT_FRAME_HEADER] = {
		(uint8_t)len, (uint8_t)(len >> 8),
		(uint8_t)timestamp, (uint8_t)(timestamp >> 8),
	};
	uint16_t index = out_head;
	for (uint16_t i = 0; i < OUT_FRAME_HEADER; i++) {
		out_ring[index] = header[i];
		index = (uint16_t)((index + 1u) & OUT_RING_MASK);
	}
	for (uint16_t i = 0; i < len; i++) {
		out_ring[index] = message[i];
		index = (uint16_t)((index + 1u) & OUT_RING_MASK);
	}
	out_head = index;
}

static bool out_peek(uint16_t *len, uint16_t *timestamp, uint8_t *first_byte)
{
	if (out_used() < OUT_FRAME_HEADER + 1u) return false;
	uint16_t index = out_tail;
	uint8_t header[OUT_FRAME_HEADER + 1u];
	for (uint16_t i = 0; i < sizeof(header); i++) {
		header[i] = out_ring[index];
		index = (uint16_t)((index + 1u) & OUT_RING_MASK);
	}
	*len = (uint16_t)(header[0] | (header[1] << 8));
	*timestamp = (uint16_t)(header[2] | (header[3] << 8));
	*first_byte = header[4];
	return true;
}

static void out_take(uint8_t *dst, uint16_t len)
{
	uint16_t index = (uint16_t)((out_tail + OUT_FRAME_HEADER) & OUT_RING_MASK);
	for (uint16_t i = 0; i < len; i++) {
		dst[i] = out_ring[index];
		index = (uint16_t)((index + 1u) & OUT_RING_MASK);
	}
	out_tail = index;
}

/* Send one complete characteristic value, waiting out controller buffer
 * exhaustion rather than dropping the MIDI it carries. */
static bool ble_midi_notify_packet(const uint8_t *packet, size_t len)
{
	if (len == 0) return true;
	for (int attempt = 0; attempt < NOTIFY_RETRY_LIMIT; attempt++) {
		if (!ble_midi_ready()) return false;
		struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, len);
		if (om != NULL) {
			int rc = ble_gatts_notify_custom(connection_handle,
			                                 midi_value_handle, om);
			if (rc == 0) return true;
			if (rc != BLE_HS_ENOMEM) {
				ESP_LOGD(TAG, "notification failed: rc=%d", rc);
				return false;
			}
		}
		vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));
	}
	ESP_LOGW(TAG, "notification abandoned after backpressure");
	return false;
}

/* A SysEx too large for a single packet is split across packets: only the
 * first carries F0 and only the last carries F7, and neither the interior
 * data bytes nor the continuation packets carry timestamps. */
static void ble_midi_notify_sysex(const uint8_t *midi, size_t len)
{
	if (len < 2 || midi[0] != 0xf0u || midi[len - 1u] != 0xf7u) return;
	size_t capacity = ble_value_capacity();
	if (capacity < 5u) return;

	uint16_t timestamp = midi_timestamp();
	uint8_t header = (uint8_t)(0x80u | ((timestamp >> 7) & 0x3fu));
	uint8_t time_low = (uint8_t)(0x80u | (timestamp & 0x7fu));
	uint8_t packet[BLE_PACKET_CAPACITY];
	size_t pos = 1; /* F0 is emitted specially in the first fragment. */
	bool first = true;

	while (true) {
		size_t used = 0;
		packet[used++] = header;
		if (first) {
			packet[used++] = time_low;
			packet[used++] = 0xf0u;
			first = false;
		}

		size_t data_left = (len - 1u) - pos; /* exclude trailing F7 */
		size_t room = capacity - used;
		if (data_left + 2u <= room) {
			memcpy(&packet[used], &midi[pos], data_left);
			used += data_left;
			packet[used++] = time_low;
			packet[used++] = 0xf7u;
			ble_midi_notify_packet(packet, used);
			return;
		}

		memcpy(&packet[used], &midi[pos], room);
		pos += room;
		used += room;
		if (!ble_midi_notify_packet(packet, used)) return;
	}
}

/* Drain the outbound queue, packing consecutive messages that share a
 * timestamp-high value into as few notifications as the MTU allows. */
static void ble_midi_drain(void)
{
	uint8_t packet[BLE_PACKET_CAPACITY];
	uint8_t message[SYSEX_CAPACITY];

	while (out_used() != 0) {
		if (!ble_midi_ready()) {
			out_reset();
			return;
		}

		size_t capacity = ble_value_capacity();
		if (capacity < 5u) return;

		uint16_t len;
		uint16_t timestamp;
		uint8_t first_byte;
		if (!out_peek(&len, &timestamp, &first_byte)) return;

		uint8_t header = (uint8_t)(0x80u | ((timestamp >> 7) & 0x3fu));
		size_t used = 0;
		packet[used++] = header;

		while (out_peek(&len, &timestamp, &first_byte)) {
			if ((uint8_t)(0x80u | ((timestamp >> 7) & 0x3fu)) != header) break;
			if (len > sizeof(message)) { /* cannot happen; stay safe */
				out_take(message, sizeof(message));
				break;
			}
			/* An inline SysEx needs a second timestamp, before its F7. */
			size_t need = first_byte == 0xf0u ? (size_t)len + 2u
			                                  : (size_t)len + 1u;
			if (used + need > capacity) break;

			out_take(message, len);
			uint8_t time_low = (uint8_t)(0x80u | (timestamp & 0x7fu));
			packet[used++] = time_low;
			if (first_byte == 0xf0u) {
				packet[used++] = 0xf0u;
				memcpy(&packet[used], &message[1], (size_t)len - 2u);
				used += (size_t)len - 2u;
				packet[used++] = time_low;
				packet[used++] = 0xf7u;
			} else {
				memcpy(&packet[used], message, len);
				used += len;
			}
		}

		if (used > 1) {
			if (!ble_midi_notify_packet(packet, used)) return;
			continue;
		}

		/* Nothing fit: the head frame is a SysEx larger than one packet. */
		if (!out_peek(&len, &timestamp, &first_byte)) return;
		if (first_byte != 0xf0u || len > sizeof(message)) {
			ESP_LOGW(TAG, "dropping unsendable %u byte message", len);
			out_take(message, len > sizeof(message) ? sizeof(message) : len);
			continue;
		}
		out_take(message, len);
		ble_midi_notify_sysex(message, len);
	}
}

static void uart_parser_emit(midi_parser_t *parser)
{
	out_push(parser->message, parser->message_len);
	parser->message_len = 0;
	parser->message_needed = 0;
}

static void uart_parser_byte(midi_parser_t *parser, uint8_t byte)
{
	/* System real-time may occur between any two bytes, including SysEx. */
	if (byte >= 0xf8u) {
		out_push(&byte, 1);
		return;
	}

	if (parser->in_sysex) {
		if (parser->sysex_len < sizeof(parser->sysex))
			parser->sysex[parser->sysex_len++] = byte;
		else {
			ESP_LOGW(TAG, "dropping overlong UART SysEx");
			parser->sysex_len = 0;
			parser->in_sysex = false;
			return;
		}
		if (byte == 0xf7u) {
			/* The PROGRAM-hold command is local to the keyboard/ESP link.
			 * Other SysEx is tapped for the editor but still reaches BLE. */
			if (!editor_control_message(parser->sysex, parser->sysex_len)) {
				sysex_bridge_offer(parser->sysex, parser->sysex_len);
				out_push(parser->sysex, parser->sysex_len);
			}
			parser->sysex_len = 0;
			parser->in_sysex = false;
		}
		return;
	}

	if (byte == 0xf0u) {
		parser->running_status = 0;
		parser->message_len = 0;
		parser->message_needed = 0;
		parser->in_sysex = true;
		parser->sysex_len = 1;
		parser->sysex[0] = byte;
		return;
	}

	if ((byte & 0x80u) != 0u) {
		/* EOX with no SysEx open -- e.g. the tail of a message discarded for
		 * being overlong. It ends nothing, and forwarding a lone F7 would
		 * look like a stray SysEx terminator to the host. */
		if (byte == 0xf7u) {
			parser->running_status = 0;
			parser->message_len = 0;
			parser->message_needed = 0;
			return;
		}

		uint8_t size = midi_message_size(byte);
		parser->message_len = 0;
		parser->message_needed = size;
		if (size == 0) {
			parser->running_status = 0;
			return;
		}
		parser->message[parser->message_len++] = byte;
		parser->running_status = byte < 0xf0u ? byte : 0;
		if (size == 1) uart_parser_emit(parser);
		return;
	}

	if (parser->message_needed == 0) {
		uint8_t size = midi_message_size(parser->running_status);
		if (size == 0) return;
		parser->message[0] = parser->running_status;
		parser->message_len = 1;
		parser->message_needed = size;
	}
	if (parser->message_len < sizeof(parser->message))
		parser->message[parser->message_len++] = byte;
	if (parser->message_len == parser->message_needed)
		uart_parser_emit(parser);
}

/*
 * Two tasks write to this UART: the NimBLE host task, forwarding MIDI
 * that arrived over BLE, and whatever task is running an editor request
 * or a firmware transfer. uart_write_bytes() is not atomic between
 * tasks, and the BLE path writes a byte at a time, so without this lock
 * a single BLE-sourced byte can land in the middle of a SysEx message.
 * The keyboard then sees a data byte with bit 7 set between F0 and F7,
 * discards the whole message, and the sender times out waiting for a
 * reply that was never going to come.
 *
 * The lock is held for a whole logical message rather than per write,
 * because the BLE path emits a running-status expansion as several
 * separate calls that must not be split either.
 */
static SemaphoreHandle_t uart_tx_lock;

static void uart_lock(void)
{
	if (uart_tx_lock != NULL) xSemaphoreTake(uart_tx_lock, portMAX_DELAY);
}

static void uart_unlock(void)
{
	if (uart_tx_lock != NULL) xSemaphoreGive(uart_tx_lock);
}

/* Callers must hold the lock above. */
static void uart_write_midi(const uint8_t *bytes, size_t len)
{
	if (len != 0) uart_write_bytes(MIDI_UART, bytes, len);
}

/* Used by sysex_bridge.c, which owns request/response but not the UART. */
void midi_uart_send(const uint8_t *bytes, size_t len)
{
	uart_lock();
	uart_write_midi(bytes, len);
	uart_unlock();
}

/* A continued SysEx packet begins with the BLE header and then a MIDI data
 * byte. Timestamp + realtime pairs are permitted before that data byte. */
static bool ble_midi_is_sysex_continuation(const uint8_t *packet, size_t len)
{
	size_t i = 1;
	while (i < len) {
		uint8_t first = packet[i++];
		if (first < 0x80u) return true;
		if (i >= len) return false;
		uint8_t second = packet[i++];
		if (second < 0x80u) return false;
		if (!midi_is_realtime(second)) return false;
	}
	return false;
}

/* Decode one complete BLE-MIDI characteristic value to ordinary serial MIDI.
 * Running status is expanded. A SysEx continuation packet has no timestamp
 * after its header; timestamps precede realtime bytes and EOX within SysEx. */
static bool ble_midi_write_to_uart_locked(const uint8_t *packet, size_t len)
{
	if (len < 2 || (packet[0] & 0x80u) == 0) return false;

	size_t i = 1;
	bool in_sysex = ble_midi_is_sysex_continuation(packet, len);
	uint8_t running_status = 0; /* BLE packet boundaries cancel running status. */

	while (i < len) {
		if (in_sysex) {
			uint8_t byte = packet[i++];
			if (byte < 0x80u) {
				uart_write_midi(&byte, 1);
				continue;
			}
			if (i >= len) return false; /* timestamp without status */
			uint8_t status = packet[i++];
			if (midi_is_realtime(status)) {
				uart_write_midi(&status, 1);
			} else if (status == 0xf7u) {
				uart_write_midi(&status, 1);
				in_sysex = false;
				running_status = 0;
			} else {
				return false;
			}
			continue;
		}

		uint8_t first = packet[i++];
		if (first < 0x80u) {
			uint8_t size = midi_message_size(running_status);
			if (size < 2 || i + (size - 2u) > len) return false;
			uint8_t message[3] = {running_status, first, 0};
			for (uint8_t n = 2; n < size; n++) {
				if (packet[i] >= 0x80u) return false;
				message[n] = packet[i++];
			}
			uart_write_midi(message, size);
			continue;
		}

		/* A status-bit byte outside SysEx is a timestamp-low byte. */
		if (i >= len) return false;
		uint8_t status_or_data = packet[i++];
		if (status_or_data < 0x80u) {
			uint8_t size = midi_message_size(running_status);
			if (size < 2 || i + (size - 2u) > len) return false;
			uint8_t message[3] = {running_status, status_or_data, 0};
			for (uint8_t n = 2; n < size; n++) {
				if (packet[i] >= 0x80u) return false;
				message[n] = packet[i++];
			}
			uart_write_midi(message, size);
			continue;
		}

		uint8_t status = status_or_data;
		if (status == 0xf0u) {
			uart_write_midi(&status, 1);
			in_sysex = true;
			running_status = 0;
			continue;
		}

		uint8_t size = midi_message_size(status);
		if (size == 0 || i + (size - 1u) > len) return false;
		uint8_t message[3] = {status, 0, 0};
		for (uint8_t n = 1; n < size; n++) {
			if (packet[i] >= 0x80u) return false;
			message[n] = packet[i++];
		}
		uart_write_midi(message, size);
		if (status < 0xf0u)
			running_status = status;
		else if (!midi_is_realtime(status) && !midi_is_system_common(status))
			running_status = 0;
	}
	return true;
}

/* Holds the UART lock across the whole packet. The decoder above has many
 * early exits, so it stays exactly as it was and the lock is taken here. */
static bool ble_midi_write_to_uart(const uint8_t *packet, size_t len)
{
	uart_lock();
	bool ok = ble_midi_write_to_uart_locked(packet, len);
	uart_unlock();
	return ok;
}

static int midi_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
			    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	(void)conn_handle;
	(void)attr_handle;
	(void)arg;
	if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;
	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

	/* The preferred MTU is BLE_PACKET_CAPACITY, so a negotiated MTU can never
	 * carry a longer write than this. Keep the buffer off the NimBLE host
	 * task's stack budget rather than sizing it for the theoretical maximum. */
	uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
	if (len > BLE_PACKET_CAPACITY) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	uint8_t packet[BLE_PACKET_CAPACITY];
	uint16_t copied = 0;
	int rc = ble_hs_mbuf_to_flat(ctxt->om, packet, sizeof(packet), &copied);
	if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
	if (!ble_midi_write_to_uart(packet, copied))
		ESP_LOGW(TAG, "ignored malformed BLE-MIDI packet");
	return 0;
}

static const struct ble_gatt_svc_def gatt_services[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &midi_service_uuid.u,
		.characteristics = (struct ble_gatt_chr_def[]) {
			{
				.uuid = &midi_characteristic_uuid.u,
				.access_cb = midi_gatt_access,
				.val_handle = &midi_value_handle,
				.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP |
				         BLE_GATT_CHR_F_NOTIFY,
			},
			{0}
		},
	},
	{0}
};

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
	struct ble_hs_adv_fields fields = {0};
	fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	fields.uuids128 = &midi_service_uuid;
	fields.num_uuids128 = 1;
	fields.uuids128_is_complete = 1;
	int rc = ble_gap_adv_set_fields(&fields);
	if (rc != 0) {
		ESP_LOGE(TAG, "advertising fields: rc=%d", rc);
		return;
	}

	struct ble_hs_adv_fields response = {0};
	const char *name = ble_svc_gap_device_name();
	response.name = (uint8_t *)name;
	response.name_len = strlen(name);
	response.name_is_complete = 1;
	rc = ble_gap_adv_rsp_set_fields(&response);
	if (rc != 0) {
		ESP_LOGE(TAG, "scan response: rc=%d", rc);
		return;
	}

	struct ble_gap_adv_params params = {0};
	params.conn_mode = BLE_GAP_CONN_MODE_UND;
	params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
	                       &params, gap_event, NULL);
	if (rc != 0) ESP_LOGE(TAG, "advertise start: rc=%d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
	(void)arg;
	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (event->connect.status == 0) {
			connection_handle = event->connect.conn_handle;
			notifications_enabled = false;
			out_reset();
			const struct ble_gap_upd_params low_latency = {
				.itvl_min = 6,  /* 7.5 ms */
				.itvl_max = 12, /* allow central to select up to 15 ms */
				.latency = 0,
				.supervision_timeout = 400,
				.min_ce_len = 0,
				.max_ce_len = 0xffff,
			};
			int rc = ble_gap_update_params(connection_handle, &low_latency);
			if (rc != 0) ESP_LOGD(TAG, "connection update: rc=%d", rc);
			ESP_LOGI(TAG, "BLE MIDI connected");
		} else {
			advertise();
		}
		return 0;
	case BLE_GAP_EVENT_DISCONNECT:
		connection_handle = INVALID_HANDLE;
		notifications_enabled = false;
		out_reset();
		ESP_LOGI(TAG, "BLE MIDI disconnected; advertising");
		advertise();
		return 0;
	case BLE_GAP_EVENT_ADV_COMPLETE:
		advertise();
		return 0;
	case BLE_GAP_EVENT_SUBSCRIBE:
		if (event->subscribe.attr_handle == midi_value_handle) {
			notifications_enabled = event->subscribe.cur_notify != 0;
			ESP_LOGI(TAG, "MIDI notifications %s",
			         notifications_enabled ? "enabled" : "disabled");
		}
		return 0;
	case BLE_GAP_EVENT_MTU:
		ESP_LOGI(TAG, "ATT MTU %u", event->mtu.value);
		return 0;
	default:
		return 0;
	}
}

static void ble_on_reset(int reason)
{
	ESP_LOGE(TAG, "NimBLE reset: %d", reason);
}

static void ble_on_sync(void)
{
	int rc = ble_hs_id_infer_auto(0, &own_addr_type);
	if (rc != 0) {
		ESP_LOGE(TAG, "address inference: rc=%d", rc);
		return;
	}
	advertise();
	ble_synced = true;
	ESP_LOGI(TAG, "advertising as MPK mini Open");
}

static void nimble_host_task(void *param)
{
	(void)param;
	nimble_port_run();
	nimble_port_freertos_deinit();
}

static void ble_tx_task(void *param)
{
	(void)param;
	while (true) {
		/* The timeout also retries a drain left unfinished by backpressure. */
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
		ble_midi_drain();
	}
}

static void midi_uart_task(void *param)
{
	(void)param;
	uint8_t bytes[64];
	while (true) {
		int count = uart_read_bytes(MIDI_UART, bytes, sizeof(bytes),
		                            pdMS_TO_TICKS(20));
		if (count <= 0) continue;
		/* Parse the whole batch before waking the sender, so messages that
		 * arrived together are packed into one notification. */
		for (int i = 0; i < count; i++) uart_parser_byte(&uart_parser, bytes[i]);
		if (out_used() != 0) xTaskNotifyGive(ble_tx_task_handle);
	}
}

static void midi_uart_init(void)
{
	uart_config_t config = {
		.baud_rate = MIDI_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	uart_tx_lock = xSemaphoreCreateMutex();
	ESP_ERROR_CHECK(uart_driver_install(MIDI_UART, 1024, 512, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(MIDI_UART, &config));
	ESP_ERROR_CHECK(uart_set_pin(MIDI_UART, MIDI_TX_GPIO, MIDI_RX_GPIO,
	                             UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

	xTaskCreate(ble_tx_task, "ble-midi-tx", 4096, NULL, 7, &ble_tx_task_handle);
	xTaskCreate(midi_uart_task, "midi-uart", 3072, NULL, 8, NULL);
}

#if STATUS_LED_ENABLE
/* Fast even blink = editor portal up (it overrides the BLE states, being
 * the transient mode you deliberately switched on). Otherwise: solid =
 * host subscribed, double blink = linked but not subscribed, single blink
 * per second = advertising. */
static void status_led_tick(void *arg)
{
	(void)arg;
	static uint8_t phase;
	phase = (uint8_t)((phase + 1u) % 10u);

	bool on;
	if (editor_active())
		on = (phase % 2u) == 0u;
	else if (ble_midi_ready())
		on = true;
	else if (connection_handle != INVALID_HANDLE)
		on = phase < 2u || (phase >= 4u && phase < 6u);
	else
		on = phase < 1u;

	gpio_set_level(STATUS_LED_GPIO, STATUS_LED_ACTIVE_LOW ? !on : on);
}

static void status_led_init(void)
{
	gpio_config_t config = {
		.pin_bit_mask = 1ULL << STATUS_LED_GPIO,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&config));
	gpio_set_level(STATUS_LED_GPIO, STATUS_LED_ACTIVE_LOW ? 1 : 0);

	const esp_timer_create_args_t timer = {
		.callback = status_led_tick,
		.name = "status-led",
	};
	esp_timer_handle_t handle;
	ESP_ERROR_CHECK(esp_timer_create(&timer, &handle));
	ESP_ERROR_CHECK(esp_timer_start_periodic(handle,
	                                         STATUS_LED_PERIOD_MS * 1000));
}

#else
static void status_led_init(void) {}
#endif

/*
 * BOOT button watcher. The button is also GPIO9's boot strapping pin, but
 * the ROM samples that at reset and this task starts long afterwards, so
 * reading it here is safe. Debounced by requiring several agreeing samples
 * rather than a timer, since nothing else depends on the latency.
 */
/*
 * Confirm a freshly installed image.
 *
 * With rollback enabled the bootloader runs a newly written slot on
 * probation and reverts to the previous one unless the app marks itself
 * valid. The test here is that the BLE host synced and started
 * advertising, and that the app then kept running for a while -- which
 * covers what a bridge with no console attached cannot recover from on
 * its own: an image that panics during startup, or one whose radio never
 * comes up. Nothing here waits on the keyboard, because the bridge only
 * talks to it when the editor portal is open and a unit that is never
 * opened would otherwise never confirm.
 */
#define CONFIRM_UPTIME_MS 15000
static void confirm_running_image(void)
{
	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_ota_img_states_t state;
	if (running == NULL) return;
	if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
	if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

	if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
		ESP_LOGI(TAG, "new image on %s confirmed", running->label);
	else
		ESP_LOGW(TAG, "could not confirm the new image; it will be rolled back");
}

static void editor_control_task(void *param)
{
	(void)param;
	gpio_config_t config = {
		.pin_bit_mask = 1ULL << EDITOR_BUTTON_GPIO,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&config));

	bool pressed = false;
	uint8_t agree = 0;
	bool confirmed = false;
	while (true) {
		uint32_t requests = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
		if (!confirmed && ble_synced &&
		    esp_timer_get_time() > (int64_t)CONFIRM_UPTIME_MS * 1000) {
			confirm_running_image();
			confirmed = true;
		}
		while (requests-- != 0u) {
			ESP_LOGI(TAG, "PROGRAM held: %s editor portal",
			         editor_active() ? "stopping" : "starting");
			editor_toggle();
		}
		bool now = gpio_get_level(EDITOR_BUTTON_GPIO) == 0; /* active low */
		if (now == pressed) {
			agree = 0;
			continue;
		}
		if (++agree < EDITOR_BUTTON_STABLE) continue;
		agree = 0;
		pressed = now;
		if (pressed) {
			ESP_LOGI(TAG, "BOOT pressed: %s editor portal",
			         editor_active() ? "stopping" : "starting");
			editor_toggle();
		}
	}
}

void app_main(void)
{
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	status_led_init();
	editor_init();
	midi_uart_init();
	xTaskCreate(editor_control_task, "editor-ctl", 3072, NULL, 4,
	            &editor_control_task_handle);
	ESP_ERROR_CHECK(nimble_port_init());

	ble_hs_cfg.reset_cb = ble_on_reset;
	ble_hs_cfg.sync_cb = ble_on_sync;
	ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
	ble_hs_cfg.sm_bonding = 1;
	ble_hs_cfg.sm_sc = 1;
	ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
	                             BLE_SM_PAIR_KEY_DIST_ID;
	ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
	                               BLE_SM_PAIR_KEY_DIST_ID;

	ble_svc_gap_init();
	ble_svc_gatt_init();
	ESP_ERROR_CHECK(ble_svc_gap_device_name_set("MPK mini Open"));
	int rc = ble_gatts_count_cfg(gatt_services);
	assert(rc == 0);
	rc = ble_gatts_add_svcs(gatt_services);
	assert(rc == 0);
	ble_store_config_init();

	nimble_port_freertos_init(nimble_host_task);
}
