/*
 * Key edge detection -> velocity-sensed MIDI Note On/Off.
 *
 * Reimplements the confirmed behavior of the original firmware's
 * FUN_08004990, fully traced (FIRMWARE_ANALYSIS.md's "Resolved: the
 * key-index lookup table" and "Follow-up: the dual-switch mechanism in
 * full" sections):
 *
 * - **Key-index mapping**: `table_index = column*4 + (bit >> 1)`,
 *   indexing a 28-byte table at flash 0x08006fcd (`00 01 02 ... 18 FF
 *   FF FF`) -- confirmed against the raw binary, not a guess.
 * - **Dual-switch velocity sensing**: row-bits 2N and 2N+1 within a
 *   column share one key index (hence `bit >> 1` above). Each key has
 *   a 3-state machine (idle/armed/fired). The odd-numbered bit arms
 *   the key and records a tick when it reaches its "released" value
 *   while idle, and sends Note Off (resetting to idle) when it reaches
 *   its "pressed" value while fired. The even-numbered bit, when it
 *   reaches its "released" value while armed, computes a tick delta
 *   since arming, fires Note On with velocity = 127 - clamp(delta, 0,
 *   126) (a plain linear inversion, not a curve -- read directly off
 *   a 127-byte table at flash 0x08006f4e), and marks the key fired.
 *   This is reproduced literally from the decompiled logic, including
 *   the (initially counterintuitive) polarity of testing each bit's
 *   *"released"* value to arm/fire rather than "pressed" -- see the
 *   FIRMWARE_ANALYSIS.md section above for why this project isn't
 *   fully certain of the physical reason (possibly reversed wiring
 *   convention on these specific velocity-sense contacts vs. the main
 *   matrix), but is confident in the logic as literally read.
 *
 * NOT CALIBRATED TO REAL TIME: the original's own reference for this
 * timing was traced to a plain wraparound counter, incremented once
 * per call to the function that contains it from elsewhere in the
 * main loop -- NOT the SysTick hardware timer (SysTick is configured
 * in the original but its interrupt handler is a no-op stub, `bx lr`,
 * confirmed by reading it directly). So this reimplementation uses an
 * equivalent free-running counter incremented once per keys_process()
 * call (i.e. once per main-loop iteration) rather than systick.c's
 * real millisecond timebase -- matching the original's actual
 * category of timing source. The absolute "feel" (how fast a press
 * needs to be for max velocity) will differ from the original if this
 * firmware's main loop runs at a different rate, but the *relative*
 * behavior (faster double-contact closure = higher velocity) is real.
 *
 * NOTE COMPUTATION: `note = key_index + program_octave()*12 +
 * program_fine_transpose() - 12`, reproduced exactly from this same
 * function's confirmed formula (record+0x02 and record+0x03 -- see
 * program.h). This replaces an earlier, self-invented placeholder
 * formula (`keys_base_note + key_index + buttons_octave_offset*12`)
 * that predated finding the original's real one. `program_octave()`
 * is live-adjustable by transport.c's octave buttons (matrix column 7,
 * confirmed -- see transport.c) -- the real octave up/down control.
 * The column-8 buttons this project originally guessed were "the"
 * octave buttons turned out to be something else entirely (pad output
 * mode select -- see buttons.c), now resolved and unrelated to note
 * pitch.
 */
#include "keys.h"
#include "matrix.h"
#include "midi_ring.h"
#include "program.h"
#include "arp.h"
#include "pads.h"
#include "transport.h"
#include "velocity.h"
#include "systick.h"

#define KEY_NONE 0xFF
#define KEY_COUNT 25

#define KEY_IDLE 0
#define KEY_ARMED 1
#define KEY_FIRED 2

/* [column 0..6][row bit 0..7] -> key index 0..24, or KEY_NONE.
 * CONFIRMED against the original's own lookup table -- see file header. */
static const uint8_t key_index_table[7][8] = {
	{0, 0, 1, 1, 2, 2, 3, 3},
	{4, 4, 5, 5, 6, 6, 7, 7},
	{8, 8, 9, 9, 10, 10, 11, 11},
	{12, 12, 13, 13, 14, 14, 15, 15},
	{16, 16, 17, 17, 18, 18, 19, 19},
	{20, 20, 21, 21, 22, 22, 23, 23},
	{24, 24, KEY_NONE, KEY_NONE, KEY_NONE, KEY_NONE, KEY_NONE, KEY_NONE},
};

static uint8_t previous_state[MATRIX_COLS];
static uint8_t key_state[KEY_COUNT];
/* Milliseconds (systick) at which each key's first contact closed. */
static uint32_t key_arm_ms[KEY_COUNT];
static uint8_t key_suppressed[KEY_COUNT];
static uint8_t key_note[KEY_COUNT];
static uint8_t key_channel[KEY_COUNT];
static uint8_t key_to_arp[KEY_COUNT];

void keys_init(void)
{
	for (int i = 0; i < MATRIX_COLS; i++) {
		previous_state[i] = 0xFFu; /* active-low matrix idle state */
	}
	for (int i = 0; i < KEY_COUNT; i++) {
		key_state[i] = KEY_IDLE;
		key_arm_ms[i] = 0;
		key_suppressed[i] = 0;
		key_note[i] = 0;
		key_channel[i] = 0;
		key_to_arp[i] = 0;
	}
}

static uint8_t note_for_key(uint8_t key_index)
{
	int16_t note = (int16_t)key_index + (int16_t)program_octave() * 12 + program_fine_transpose() - 12;
	if (note < 0) {
		note = 0;
	}
	if (note > 127) {
		note = 127;
	}
	return (uint8_t)note;
}

static void send_note(uint8_t key_index, uint8_t on, uint8_t velocity)
{
	if (on && (transport_program_held() || transport_arp_held())) {
		key_suppressed[key_index] = 1;
		if (transport_program_held() && key_index >= 21 && key_index <= 24) {
			keys_all_off();
			pads_all_off();
			arp_all_off();
			program_select((uint8_t)(key_index - 20));
		} else if (transport_arp_held()) {
			if (key_index < 8) {
				program_set_arp_clock_div(key_index);
				transport_mark_arp_setting_used();
			} else if (key_index >= 9 && key_index <= 14) {
				static const uint8_t mode_map[6] = {0, 1, 3, 2, 5, 4};
				program_set_arp_mode(mode_map[key_index - 9]);
				transport_mark_arp_setting_used();
			} else if (key_index >= 16 && key_index <= 19) {
				program_set_arp_range((uint8_t)(key_index - 16));
				transport_mark_arp_setting_used();
			}
		}
		return;
	}
	if (!on && key_suppressed[key_index]) {
		key_suppressed[key_index] = 0;
		return;
	}

	if (on) {
		key_note[key_index] = note_for_key(key_index);
		key_channel[key_index] = program_channel();
		key_to_arp[key_index] = program_arp_enabled();
	}
	uint8_t note = key_note[key_index];
	uint8_t channel = key_channel[key_index];
	if (key_to_arp[key_index]) {
		/* Feed the arpeggiator instead of sending directly -- arp.c
		 * generates its own Note On/Off stream from the held-note set.
		 * This is the intended wiring point flagged in arp.c's header
		 * as "not wired up yet". */
		if (on) {
			arp_note_on(note, velocity);
		} else {
			arp_note_off(note);
		}
		return;
	}

	uint8_t event[4];
	event[0] = on ? 0x09 : 0x08; /* Cable 0, CIN: Note On / Note Off */
	event[1] = (uint8_t)((on ? 0x90 : 0x80) | channel);
	event[2] = note;
	event[3] = on ? velocity : 127;
	midi_ring_push(event, 4);

}

void keys_all_off(void)
{
	for (uint8_t i = 0; i < KEY_COUNT; i++) {
		if (key_state[i] == KEY_FIRED && !key_suppressed[i]) {
			if (key_to_arp[i]) {
				arp_note_off(key_note[i]);
			} else {
				uint8_t event[4] = {0x08, (uint8_t)(0x80 | key_channel[i]), key_note[i], 127};
				midi_ring_push(event, 4);
			}
		}
		key_state[i] = KEY_IDLE;
		key_suppressed[i] = 0;
	}
}

static void handle_bit(uint8_t key_index, int bit, uint8_t released)
{
	if (key_index == KEY_NONE) {
		return;
	}

	if (bit & 1) {
		/* Odd sub-switch: arm on reaching "released", Note Off on
		 * reaching "pressed" while fired. */
		if (released) {
			if (key_state[key_index] == KEY_IDLE) {
				key_state[key_index] = KEY_ARMED;
				key_arm_ms[key_index] = systick_millis();
			}
		} else {
			if (key_state[key_index] == KEY_FIRED) {
				send_note(key_index, 0, 0);
			}
			key_state[key_index] = KEY_IDLE;
		}
	} else {
		/* Even sub-switch: fire Note On on reaching "released" while
		 * armed, velocity from the tick delta since arming. */
		if (released && key_state[key_index] == KEY_ARMED) {
			/* Real elapsed time, not main-loop iterations. The original
			 * counted iterations, which tied the feel to whatever rate
			 * its loop happened to run at; this loop is quicker, so an
			 * ordinary press overran the 126-count range and pinned
			 * every note to velocity 1. See velocity.h. */
			uint32_t delta_ms = systick_millis() - key_arm_ms[key_index];
			program_note_velocity_interval(delta_ms);

			uint8_t velocity = velocity_from_interval(delta_ms,
			                                          program_key_fast_ms(),
			                                          program_key_slow_ms());
			velocity = velocity_apply(program_key_curve(), velocity,
			                          program_key_fixed_velocity());
			key_state[key_index] = KEY_FIRED;
			send_note(key_index, 1, velocity);
		}
	}
}

void keys_process(void)
{
	/* Columns 0-6 carry the 7x8 key matrix per FIRMWARE_ANALYSIS.md;
	 * columns 7-8 are special-cased in the original for a smaller
	 * number of inputs (transport/other buttons -- see buttons.c). */
	for (int col = 0; col < 7; col++) {
		uint8_t now = matrix_state[col];
		uint8_t prev = previous_state[col];
		uint8_t changed = now ^ prev;

		if (changed == 0) {
			continue;
		}

		for (int bit = 0; bit < 8; bit++) {
			uint8_t mask = (uint8_t)(1u << bit);
			if ((changed & mask) == 0) {
				continue;
			}
			uint8_t key_index = key_index_table[col][bit];
			uint8_t released = (now & mask) == 0;
			handle_bit(key_index, bit, released);
		}

		previous_state[col] = now;
	}
}
