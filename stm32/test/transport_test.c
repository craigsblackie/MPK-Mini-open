#include <stdint.h>
#include <stdio.h>

#include "transport.h"

#define BIT_PROGRAM (1u << 3)

static uint32_t now_ms;
static int toggle_count;
static int failures;

uint32_t systick_millis(void) { return now_ms; }
void midi_uart_editor_toggle(void) { toggle_count++; }
void keys_all_off(void) {}
void arp_all_off(void) {}
void arp_tap(void) {}
uint8_t program_channel(void) { return 0; }
uint8_t program_arp_enabled(void) { return 0; }
void program_toggle_arp_enabled(void) {}
void program_toggle_arp_latched(void) {}
uint8_t program_octave(void) { return 4; }
void program_set_octave(uint8_t octave) { (void)octave; }

/* transport.c emits sustain through the normal MIDI ring; it is unrelated to
 * this test, but the symbol is still part of the linked translation unit. */
void midi_ring_push(const uint8_t *data, uint8_t len)
{
	(void)data;
	(void)len;
}

static void check(const char *name, int condition)
{
	printf("%s %s\n", condition ? "ok  " : "FAIL", name);
	if (!condition) failures++;
}

int main(void)
{
	transport_init();
	now_ms = 100;
	transport_process(BIT_PROGRAM);
	now_ms = 2099;
	transport_process(BIT_PROGRAM);
	check("short PROGRAM hold does not toggle", toggle_count == 0);

	now_ms = 2100;
	transport_process(BIT_PROGRAM);
	check("two-second PROGRAM hold toggles", toggle_count == 1);

	now_ms = 5000;
	transport_process(BIT_PROGRAM);
	check("one toggle per hold", toggle_count == 1);

	transport_process(0);
	now_ms = 6000;
	transport_process(BIT_PROGRAM);
	transport_mark_program_setting_used();
	now_ms = 8000;
	transport_process(BIT_PROGRAM);
	check("PROGRAM+key does not toggle", toggle_count == 1);

	transport_process(0);
	now_ms = 9000;
	transport_process(BIT_PROGRAM);
	now_ms = 11000;
	transport_process(BIT_PROGRAM);
	check("release rearms the gesture", toggle_count == 2);

	printf("\n%s\n", failures ? "FAILURES PRESENT" : "all transport tests passed");
	return failures != 0;
}
