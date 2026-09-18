#ifndef PROGRAM_H
#define PROGRAM_H

#include <stdint.h>

/*
 * Per-program configuration record.
 *
 * Layout is the original firmware's own 101-byte per-program record,
 * decoded from FIRMWARE_ANALYSIS.md's "Major new finding: 101-byte
 * per-program record layout" and cross-confirmed against the actual
 * consumer functions (the knob and pad handlers) -- not a from-scratch
 * design. PROGRAM_COUNT (5) matches the original's own range check
 * (FUN_08005ac8 validates `param_1 < 5`).
 */
#define PROGRAM_COUNT 5
#define PROGRAM_RECORD_SIZE 101

#define PAD_MODE_NOTE 1
#define PAD_MODE_CC 2
#define PAD_MODE_PC 3

typedef struct {
	uint8_t raw[PROGRAM_RECORD_SIZE];
} program_record_t;

extern program_record_t programs[PROGRAM_COUNT];
extern uint8_t current_program;

void program_init(void);
void program_select(uint8_t program_index);
void program_persist(void);
void program_reset_scratch(void);
uint8_t program_factory_reset_performed(void);

/* record+0x00: MIDI channel (0-15), confirmed shared by the knob
 * handler. Reused here for pads too (not independently confirmed for
 * pads -- a reasonable assumption of one MIDI channel per program). */
uint8_t program_channel(void);
uint8_t program_pad_channel(void);

/* record+0x04: arp on/off. record+0x06: arp clock-division selector
 * (0-7), see FIRMWARE_ANALYSIS.md's confirmed tick/step table.
 * record+0x0a/+0x0b: tempo (BPM), combined per the original's own
 * validation formula. record+0x05: arp mode (0-5) -- confirmed via
 * FUN_08002588's 6-case switch: 0=up, 1=down (factory default), 2=up-
 * down, 3=down-up, 4=random (confirmed LCG, `x = x*0x6255 + 0x3619`),
 * 5=shape-identical to up but bounded against a different held-note
 * count (`DAT_08002c98` vs. `DAT_0800299c` elsewhere in that function)
 * -- plausibly an "order played" mode, not independently confirmed;
 * see ARP_MODE_* below and FIRMWARE_ANALYSIS.md. record+0x0c: arp
 * octave range (0-3 additional octave repeats above the base pass).
 * The six mode values are 0=Up, 1=Down, 2=Exclusive, 3=Inclusive,
 * 4=Random, and 5=Order. */
uint8_t program_arp_enabled(void);
/* Confirmed a real button toggles this directly (FUN_08006988 bit 0,
 * a literal `flag = (flag == 0)` boolean flip) -- see transport.c. */
void program_toggle_arp_enabled(void);
uint8_t program_arp_clock_div(void);
void program_set_arp_clock_div(uint8_t division);
uint16_t program_tempo_bpm(void);
uint8_t program_arp_mode(void);
void program_set_arp_mode(uint8_t mode);
uint8_t program_arp_range(void);
void program_set_arp_range(uint8_t range);
uint8_t program_arp_external_clock(void);
uint8_t program_arp_latched(void);
void program_toggle_arp_latched(void);
uint8_t program_tap_count(void);

/* record+0x02/+0x03: the keyboard's base-note transpose, confirmed via
 * FUN_08004990's note computation: `key + octave*12 + fine - 12`.
 * record+0x02 (0-8, factory default 4) is octave-scale, while
 * record+0x03 (0-24, factory default 12) is a finer transpose
 * within that. Confirmed via `FUN_08006988`'s transport-button
 * handler (see FIRMWARE_ANALYSIS.md) that record+0x02 specifically is
 * live-adjustable by two dedicated buttons (increment/decrement,
 * clamped 0-8) with a reset-to-4 combo -- i.e. these are the real
 * octave up/down buttons, on matrix column 7. Column 8 selects pad bank
 * and output mode. */
uint8_t program_octave(void);
uint8_t program_fine_transpose(void);
void program_set_octave(uint8_t octave); /* clamped 0-8 */

#define ARP_MODE_UP 0
#define ARP_MODE_DOWN 1
#define ARP_MODE_EXCLUSIVE 2
#define ARP_MODE_INCLUSIVE 3
#define ARP_MODE_RANDOM 4
#define ARP_MODE_ORDER 5

/* record+0x4d + knob*3: knob CC number (0 = knob unassigned, per the
 * original's confirmed "gate byte doubles as CC number" behavior). */
uint8_t program_knob_cc(uint8_t knob);
uint8_t program_knob_low(uint8_t knob);
uint8_t program_knob_high(uint8_t knob);

/* record+0x0d + pad*8: pad sub-record. +0/+1 are Bank A/B note#,
 * +2/+3 are Bank A/B program-change numbers, and +4/+5 are Bank A/B
 * CC numbers. Which pair is used depends on the active pad output mode. */
uint8_t program_pad_note(uint8_t pad);
uint8_t program_pad_pc(uint8_t pad);
uint8_t program_pad_cc(uint8_t pad);
uint8_t program_pad_toggle(uint8_t pad);

/* CONFIRMED NOT part of the per-program record -- a single shared
 * runtime variable (SRAM 0x20000023), confirmed via get_xrefs_to: read
 * by FUN_08003ab8 (pad velocity, as `*DAT_08003e18` -- the same
 * address under a different literal-pool alias) and written by matrix
 * column 8's buttons (`FUN_080044fc`, as `DAT_080045c0+2` -- see
 * buttons.c). This resolves buttons.c's real purpose: its bit 2/3
 * buttons toggle this between PAD_MODE_CC/PAD_MODE_PC and
 * PAD_MODE_NOTE (press to enter that mode, press again to return to
 * Note) -- the actual real-world feature this project had been
 * guessing at as "octave buttons" before column 7 was traced. Returns
 * PAD_MODE_NOTE/CC/PC. */
uint8_t program_pad_mode(void);
void program_set_pad_mode(uint8_t mode);

/* Shared runtime pad bank selected by the two column-8 bank buttons.
 * The stock LED byte uses bit 0 for bank A and bit 1 for bank B. */
uint8_t program_pad_bank(void);
void program_set_pad_bank(uint8_t bank);

/*
 * SysEx wire <-> record conversion.
 *
 * Reproduces the exact byte-reorder table read directly out of the
 * original firmware's SysEx 'a' (write program) and 'c' (dump program)
 * command handlers (FUN_08002eac) -- FIRMWARE_ANALYSIS.md's "Follow-up
 * pass: full command set and wire encoding" section has the full
 * derivation. It's a pure reorder (same 101 bytes both ways), not a
 * bit-packing scheme, despite an earlier pass of that document
 * guessing otherwise before the function was read in full.
 *
 * wire[] here is just the 101-byte payload -- the 8-byte message
 * header (F0 47 <id> 7C <cmd> <len_hi> <len_lo> <program#>) and
 * trailing F7 are sysex.c's concern, not this module's.
 */
void program_load_from_wire(uint8_t program_index, const uint8_t wire[PROGRAM_RECORD_SIZE]);
void program_save_to_wire(uint8_t program_index, uint8_t wire[PROGRAM_RECORD_SIZE]);

/*
 * Global settings block -- an addition, with no counterpart in the
 * original firmware.
 *
 * These are NOT in the 101-byte record: every byte of that record is
 * already spoken for by the original's layout, and it is what the stock
 * editor reads and writes, so anything stored there would be clobbered
 * by a stock-editor program write. The block lives at its own offset in
 * the same flash page as the program store, past the 407 bytes the
 * stock-compatible image occupies, behind a magic and a version so an
 * older device (or a fresh page) reads as "no settings" and falls back
 * to values that reproduce stock behaviour exactly.
 *
 * program_persist() erases the whole page, so it writes both areas in
 * one pass -- settings survive a program save and vice versa.
 */
#define SETTINGS_PAYLOAD_SIZE 6

uint8_t program_key_curve(void);
uint8_t program_pad_curve(void);
uint8_t program_key_fixed_velocity(void);
uint8_t program_pad_fixed_velocity(void);

/* Key velocity window, in milliseconds between the two contacts: the
 * interval a hard strike produces (mapped to 127) and the gentlest
 * playable press (mapped to 1). Tunable at runtime because the right
 * values are a property of the keybed, not of this code -- see
 * program_velocity_stats() for measuring them on a real instrument. */
uint8_t program_key_fast_ms(void);
uint8_t program_key_slow_ms(void);

/* Calibration telemetry: the shortest, longest and most recent contact
 * intervals seen since boot, plus how many keys have been struck
 * (each saturating at 127 so it stays a 7-bit SysEx payload). Feed a
 * minute of ordinary playing in, then set the window from what comes
 * out. Reading does not reset them. */
void program_note_velocity_interval(uint32_t delta_ms);
void program_velocity_stats(uint8_t out[4]);

/* Pack/unpack the 7-bit-safe SysEx payload. Out-of-range values are
 * clamped rather than rejected, so a partially understood payload from a
 * newer editor still leaves the device in a usable state. */
void program_settings_to_wire(uint8_t wire[SETTINGS_PAYLOAD_SIZE]);
void program_settings_from_wire(const uint8_t wire[SETTINGS_PAYLOAD_SIZE]);

#endif /* PROGRAM_H */
