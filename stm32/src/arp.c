/* MPK mini mk1 arpeggiator: six modes, latch, tap average, internal/external clock. */
#include "arp.h"
#include "midi_ring.h"
#include "program.h"
#include "systick.h"

typedef struct { uint8_t note, velocity, held; } arp_note_t;
static const uint8_t division_ticks[8] = {24,16,12,8,6,4,3,2};
static const uint8_t gate_ticks[8] = {12,8,6,4,3,2,1,1};

uint8_t arp_enabled = 1;
static arp_note_t notes[ARP_MAX_NOTES];
static uint8_t note_count, sequence_pos, octave_pass, random_steps;
static int8_t direction;
static uint16_t rng_state;
static uint8_t running, last_note_active, last_note, last_enabled, last_latch;
static uint32_t next_step_ms, note_off_ms;
static uint8_t ext_running, ext_tick;
static uint32_t last_clock_ms, ext_quarter_ms;
static uint32_t last_tap_ms;
static uint16_t tap_intervals[4], tapped_period_ms;
static uint8_t tap_intervals_valid, tap_ring;

static void send_event(uint8_t on, uint8_t note, uint8_t velocity)
{
	uint8_t e[4] = {on ? 0x09 : 0x08,
	                (uint8_t)((on ? 0x90 : 0x80) | program_channel()),
	                note, on ? velocity : 127};
	midi_ring_push(e, 4);
}

static void stop_sounding_note(void)
{
	if (last_note_active) {
		send_event(0, last_note, 127);
		last_note_active = 0;
	}
}

void arp_init(void)
{
	arp_enabled = 1; note_count = 0; sequence_pos = 0; direction = 1;
	octave_pass = 0; rng_state = 1; random_steps = 0; running = 0;
	last_note_active = 0; last_enabled = program_arp_enabled();
	last_latch = program_arp_latched(); next_step_ms = 0; note_off_ms = 0;
	ext_running = 0; ext_tick = 0; last_clock_ms = 0; ext_quarter_ms = 0;
	last_tap_ms = 0; tap_intervals_valid = 0; tap_ring = 0; tapped_period_ms = 0;
}

static void remove_note_at(uint8_t index)
{
	for (uint8_t i = index; i + 1 < note_count; i++) notes[i] = notes[i + 1];
	if (note_count) note_count--;
	if (sequence_pos >= note_count) sequence_pos = 0;
}

static uint8_t physical_count(void)
{
	uint8_t count = 0;
	for (uint8_t i = 0; i < note_count; i++) if (notes[i].held) count++;
	return count;
}

void arp_note_on(uint8_t note, uint8_t velocity)
{
	for (uint8_t i = 0; i < note_count; i++) {
		if (notes[i].note == note) { notes[i].velocity = velocity; notes[i].held = 1; return; }
	}
	if (program_arp_latched() && physical_count() == 0 && note_count != 0) {
		stop_sounding_note(); note_count = 0; sequence_pos = 0; running = 0;
	}
	if (note_count < ARP_MAX_NOTES) {
		notes[note_count].note = note; notes[note_count].velocity = velocity;
		notes[note_count].held = 1; note_count++;
	}
}

void arp_note_off(uint8_t note)
{
	for (uint8_t i = 0; i < note_count; i++) if (notes[i].note == note) {
		notes[i].held = 0;
		if (!program_arp_latched()) remove_note_at(i);
		return;
	}
}

static void release_unheld(void)
{
	for (uint8_t i = 0; i < note_count;) {
		if (!notes[i].held) remove_note_at(i); else i++;
	}
}

void arp_all_off(void)
{
	stop_sounding_note(); note_count = 0; running = 0; ext_running = 0; ext_tick = 0;
}

void arp_tap(void)
{
	uint32_t now = systick_millis();
	if (last_tap_ms != 0 && now - last_tap_ms < 6001u) {
		tap_intervals[tap_ring] = (uint16_t)(now - last_tap_ms);
		tap_ring = (uint8_t)((tap_ring + 1) % program_tap_count());
		if (tap_intervals_valid < program_tap_count()) tap_intervals_valid++;
		if (tap_intervals_valid >= program_tap_count()) {
			uint32_t total = 0;
			for (uint8_t i = 0; i < program_tap_count(); i++) total += tap_intervals[i];
			uint32_t average = total / program_tap_count();
			if (average < 250) average = 250;
			if (average > 2000) average = 2000;
			tapped_period_ms = (uint16_t)average;
		}
	} else { tap_intervals_valid = 0; tap_ring = 0; }
	last_tap_ms = now;
}

uint32_t arp_beat_interval_ms(void)
{
	if (program_arp_external_clock() && ext_quarter_ms) return ext_quarter_ms;
	if (tapped_period_ms) return tapped_period_ms;
	return 60000u / program_tempo_bpm();
}

static uint32_t step_interval_ms(void)
{
	return (arp_beat_interval_ms() * division_ticks[program_arp_clock_div()]) / 24u;
}

static uint8_t nth_by_pitch(uint8_t n, uint8_t descending)
{
	uint8_t used[ARP_MAX_NOTES], selected = 0;
	for (uint8_t i = 0; i < ARP_MAX_NOTES; i++) used[i] = 0;
	for (uint8_t rank = 0; rank <= n; rank++) {
		uint8_t best = 0xff;
		for (uint8_t i = 0; i < note_count; i++) {
			if (used[i]) continue;
			if (best == 0xff || (!descending && notes[i].note < notes[best].note) ||
			    (descending && notes[i].note > notes[best].note)) best = i;
		}
		selected = best; used[best] = 1;
	}
	return selected;
}

static uint16_t rng_next(void) { rng_state = (uint16_t)(rng_state * 0x6255u + 0x3619u); return rng_state; }

static uint8_t selected_entry(void)
{
	uint8_t mode = program_arp_mode();
	if (mode == ARP_MODE_ORDER) return sequence_pos;
	if (mode == ARP_MODE_RANDOM) return (uint8_t)(rng_next() % note_count);
	return nth_by_pitch(sequence_pos, mode == ARP_MODE_DOWN);
}

static void start_sequence(void)
{
	sequence_pos = 0; direction = 1;
	octave_pass = program_arp_mode() == ARP_MODE_DOWN ? program_arp_range() : 0;
	random_steps = 0; running = 1;
}

static void advance_octave_up(void) { if (++octave_pass > program_arp_range()) octave_pass = 0; }

static void advance_sequence(void)
{
	uint8_t mode = program_arp_mode();
	if (note_count < 2 && (mode == ARP_MODE_EXCLUSIVE || mode == ARP_MODE_INCLUSIVE)) {
		advance_octave_up();
		return;
	}
	if (mode == ARP_MODE_RANDOM) {
		if (++random_steps >= note_count) { random_steps = 0; advance_octave_up(); }
		return;
	}
	if (mode == ARP_MODE_DOWN) {
		if (++sequence_pos >= note_count) {
			sequence_pos = 0;
			if (octave_pass == 0) octave_pass = program_arp_range(); else octave_pass--;
		}
		return;
	}
	if (mode == ARP_MODE_EXCLUSIVE || mode == ARP_MODE_INCLUSIVE) {
		uint8_t inclusive = mode == ARP_MODE_INCLUSIVE;
		if (direction > 0) {
			if (sequence_pos + 1 < note_count) sequence_pos++;
			else { direction = -1; if (!inclusive) sequence_pos--; }
		} else if (sequence_pos > 0) sequence_pos--;
		else { direction = 1; advance_octave_up(); if (!inclusive) sequence_pos++; }
		return;
	}
	if (++sequence_pos >= note_count) { sequence_pos = 0; advance_octave_up(); }
}

static void play_step(void)
{
	if (!note_count) return;
	if (!running) start_sequence();
	stop_sounding_note();
	if (sequence_pos >= note_count) sequence_pos = 0;
	uint8_t entry = selected_entry();
	uint16_t note = (uint16_t)notes[entry].note + (uint16_t)octave_pass * 12u;
	if (note < 128) {
		send_event(1, (uint8_t)note, notes[entry].velocity);
		last_note = (uint8_t)note; last_note_active = 1;
	}
	advance_sequence();
}

void arp_midi_realtime(uint8_t byte)
{
	if (!program_arp_external_clock()) return;
	if (byte == 0xfa) { ext_running = 1; ext_tick = 0; running = 0; }
	else if (byte == 0xfb) ext_running = 1;
	else if (byte == 0xfc) { ext_running = 0; stop_sounding_note(); }
	else if (byte == 0xf8 && ext_running && arp_enabled && program_arp_enabled()) {
		uint32_t now = systick_millis();
		if (last_clock_ms) { uint32_t delta = now - last_clock_ms; if (delta) ext_quarter_ms = delta * 24u; }
		last_clock_ms = now;
		uint8_t division = program_arp_clock_div();
		if (ext_tick == 0) play_step();
		if (last_note_active && ext_tick == gate_ticks[division]) stop_sounding_note();
		if (++ext_tick >= division_ticks[division]) ext_tick = 0;
	}
}

void arp_process(void)
{
	uint8_t enabled = arp_enabled && program_arp_enabled();
	uint8_t latch = program_arp_latched();
	if (last_latch && !latch) release_unheld();
	last_latch = latch;
	if (last_enabled && !enabled) arp_all_off();
	last_enabled = enabled;
	if (!enabled || note_count == 0) { stop_sounding_note(); running = 0; return; }
	if (program_arp_external_clock()) return;
	uint32_t now = systick_millis();
	if (last_note_active && (int32_t)(now - note_off_ms) >= 0) stop_sounding_note();
	if (!running || (int32_t)(now - next_step_ms) >= 0) {
		uint32_t interval = step_interval_ms();
		play_step(); next_step_ms = now + interval; note_off_ms = now + interval / 2u;
	}
}
