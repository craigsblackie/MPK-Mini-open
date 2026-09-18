#include "midi_ring.h"
#include "midi_uart.h"

static uint8_t buf[MIDI_RING_SIZE];
static volatile size_t head; /* write index */
static volatile size_t tail; /* read index */
static volatile size_t used;

void midi_ring_init(void)
{
	head = 0;
	tail = 0;
	used = 0;
}

int midi_ring_push(const uint8_t *data, size_t len)
{
	/* Bluetooth remains independent of USB configuration/backpressure. */
	midi_uart_mirror_usb(data, len);
	if (used + len > MIDI_RING_SIZE) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		buf[head] = data[i];
		head = (head + 1) % MIDI_RING_SIZE;
	}
	used += len;
	return 1;
}

size_t midi_ring_drain(uint8_t *dst, size_t max_len)
{
	size_t n = used < max_len ? used : max_len;
	for (size_t i = 0; i < n; i++) {
		dst[i] = buf[tail];
		tail = (tail + 1) % MIDI_RING_SIZE;
	}
	used -= n;
	return n;
}

size_t midi_ring_count(void)
{
	return used;
}
