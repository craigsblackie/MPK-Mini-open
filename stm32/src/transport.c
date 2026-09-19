/*
 * Matrix column 7: Arp modifier/toggle, Tap Tempo, Sustain/arp latch,
 * Program modifier, and Octave Down/Up.
 *
 * Reimplements behavior found by fully tracing `FUN_08006988` (live
 * Ghidra access) -- a function this project had earlier only
 * partially characterized as a "tap-tempo candidate, lowest
 * confidence." Reading it in full resolves much more:
 *
 * - Its input byte is confirmed, via `get_xrefs_to` on the SRAM
 *   address, to be written by the matrix scanner at **column 7**
 *   (`0x20000012`, `pcVar2[2]` in the scanner -- the *other* "extra"
 *   column, distinct from column 8's cluster in buttons.c).
 * - **Bit 2** (mask 0x04) is tracked by level (not edge) and sends
 *   Control Change 64 -- the standard MIDI **sustain pedal** CC --
 *   value 127 while held, 0 on release. Unambiguous: CC 64 with a
 *   momentary press/release shape is definitionally a sustain pedal.
 * - **Bits 4 and 5** (0x10, 0x20) directly increment/decrement
 *   record+0x02 (clamped 0-8), with holding both together (0x30)
 *   resetting it to 4 (the factory default). record+0x02 is the same
 *   field `FUN_08004990` uses as the keyboard's real octave in its
 *   note formula (see program.h's `program_octave()`) -- so these are
 *   the **real octave up/down buttons**, on column 7, not column 8 as
 *   this project originally guessed (see buttons.c's header for that
 *   reinterpretation).
 * - **Bit 1** (mask 0x02) drives tap tempo. Intervals are averaged over
 *   record+0x09 samples and clamped to 250-2000 ms.
 * - **Bit 0** (mask 0x01): on release (transitioning away from a
 *   lone bit-0 press), the original does a literal `record[4] =
 *   (record[4] == 0)` -- an unconditional boolean flip of the arp
 *   on/off flag. Unambiguous: this is the **arp on/off toggle
 *   button**. Reimplemented as a simple edge-triggered toggle (the
 * - **Bit 3** (mask 0x08) is the Program modifier. Program plus the
 *   four highest keys selects stored programs 1-4.
 */
#include "transport.h"
#include "midi_ring.h"
#include "program.h"
#include "arp.h"
#include "keys.h"
#include "midi_uart.h"
#include "systick.h"

#define BIT_ARP_TOGGLE (1u << 0)
#define BIT_TAP (1u << 1)
#define BIT_SUSTAIN (1u << 2)
#define BIT_PROGRAM (1u << 3)
#define BIT_OCTAVE_DOWN (1u << 4)
#define BIT_OCTAVE_UP (1u << 5)
#define BOTH_OCTAVE_BITS (BIT_OCTAVE_DOWN | BIT_OCTAVE_UP)
#define EDITOR_HOLD_MS 2000u

static uint8_t previous_status;
static uint8_t arp_setting_used;
static uint8_t program_setting_used;
static uint8_t editor_toggle_sent;
static uint32_t program_press_ms;

void transport_init(void)
{
	previous_status = 0;
	arp_setting_used = 0;
	program_setting_used = 0;
	editor_toggle_sent = 0;
	program_press_ms = 0;
}

static void send_sustain(uint8_t on)
{
	uint8_t event[4];
	event[0] = 0x0B; /* Cable 0, CIN: Control Change */
	event[1] = (uint8_t)(0xB0 | program_channel());
	event[2] = 64; /* standard MIDI sustain pedal CC */
	event[3] = on ? 127 : 0;
	midi_ring_push(event, 4);
}

void transport_process(uint8_t status_byte)
{
	uint8_t changed = status_byte ^ previous_status;
	previous_status = status_byte;

	if (changed & BIT_ARP_TOGGLE) {
		if (status_byte & BIT_ARP_TOGGLE) {
			arp_setting_used = 0;
		} else {
			arp_all_off();
			if (!arp_setting_used) program_toggle_arp_enabled();
		}
	}
	if ((changed & BIT_PROGRAM) && (status_byte & BIT_PROGRAM)) {
		keys_all_off();
		arp_all_off();
		program_press_ms = systick_millis();
		program_setting_used = 0;
		editor_toggle_sent = 0;
	}
	if ((changed & BIT_PROGRAM) && !(status_byte & BIT_PROGRAM)) {
		program_setting_used = 0;
		editor_toggle_sent = 0;
	}
	if ((status_byte & BIT_PROGRAM) && !program_setting_used &&
	    !editor_toggle_sent &&
	    (uint32_t)(systick_millis() - program_press_ms) >= EDITOR_HOLD_MS) {
		midi_uart_editor_toggle();
		editor_toggle_sent = 1;
	}

	if (changed & BIT_SUSTAIN) {
		if (program_arp_enabled()) {
			if (status_byte & BIT_SUSTAIN) program_toggle_arp_latched();
		} else {
			send_sustain((status_byte & BIT_SUSTAIN) != 0);
		}
	}

	if (changed & BIT_TAP) {
		if (status_byte & BIT_TAP) {
			arp_tap();
		}
	}

	if (changed & BOTH_OCTAVE_BITS) {
		if ((status_byte & BOTH_OCTAVE_BITS) == BOTH_OCTAVE_BITS) {
			program_set_octave(4); /* confirmed factory default */
		} else if (status_byte & BIT_OCTAVE_UP) {
			uint8_t o = program_octave();
			if (o < 8) {
				program_set_octave((uint8_t)(o + 1));
			}
		} else if (status_byte & BIT_OCTAVE_DOWN) {
			uint8_t o = program_octave();
			if (o > 0) {
				program_set_octave((uint8_t)(o - 1));
			}
		}
	}
}

uint8_t transport_program_held(void) { return (previous_status & BIT_PROGRAM) != 0; }
uint8_t transport_arp_held(void) { return (previous_status & BIT_ARP_TOGGLE) != 0; }
void transport_mark_arp_setting_used(void) { arp_setting_used = 1; }
void transport_mark_program_setting_used(void) { program_setting_used = 1; }
