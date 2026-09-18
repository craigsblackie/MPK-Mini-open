/* Stock-compatible piezo envelope, pad messages, toggles, and LED state. */
#include "pads.h"
#include "adc.h"
#include "midi_ring.h"
#include "program.h"
#include "velocity.h"

#define ATTACK_THRESHOLD 0x80u
#define RELEASE_THRESHOLD 0x41u
#define PEAK_LIMIT 0x2a0u
#define ATTACK_SAMPLES 5u
#define RELEASE_HOLD 30u

typedef struct {
	uint8_t active;
	uint8_t attack_count;
	uint8_t release_count;
	uint16_t peak;
	uint8_t sent_mode;
	uint8_t sent_bank;
	uint8_t sent_number;
	uint8_t sent_channel;
} pad_envelope_t;

static pad_envelope_t envelope[PADS_NUM];
/* [Note/CC/PC][bank A/B][pad]. */
static uint8_t output_active[3][2][PADS_NUM];
static uint32_t last_generation;

static void push_note(uint8_t channel, uint8_t note, uint8_t on, uint8_t velocity)
{
	uint8_t e[4] = {on ? 0x09 : 0x08,
	                (uint8_t)((on ? 0x90 : 0x80) | channel), note, velocity};
	midi_ring_push(e, 4);
}

static void push_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
	uint8_t e[4] = {0x0b, (uint8_t)(0xb0 | channel), cc, value};
	midi_ring_push(e, 4);
}

static void push_pc(uint8_t channel, uint8_t pc)
{
	uint8_t e[4] = {0x0c, (uint8_t)(0xc0 | channel), pc, 0};
	midi_ring_push(e, 4);
}

static void pad_hit(uint8_t pad, uint8_t velocity)
{
	pad_envelope_t *p = &envelope[pad];
	uint8_t mode = program_pad_mode();
	uint8_t bank = program_pad_bank();
	uint8_t channel = program_pad_channel();
	uint8_t toggle = program_pad_toggle(pad);
	uint8_t *lit = &output_active[mode - 1][bank][pad];
	p->sent_mode = mode;
	p->sent_bank = bank;
	p->sent_channel = channel;

	if (mode == PAD_MODE_NOTE) {
		p->sent_number = program_pad_note(pad);
		if (toggle && *lit) {
			push_note(channel, p->sent_number, 0, 127);
			*lit = 0;
		} else {
			push_note(channel, p->sent_number, 1, velocity);
			*lit = 1;
		}
	} else if (mode == PAD_MODE_CC) {
		p->sent_number = program_pad_cc(pad);
		if (toggle && *lit) {
			push_cc(channel, p->sent_number, 0);
			*lit = 0;
		} else {
			push_cc(channel, p->sent_number, velocity);
			*lit = 1;
		}
	} else {
		p->sent_number = program_pad_pc(pad);
		push_pc(channel, p->sent_number);
		*lit = 1;
	}
}

static void pad_release(uint8_t pad)
{
	pad_envelope_t *p = &envelope[pad];
	uint8_t *lit = &output_active[p->sent_mode - 1][p->sent_bank][pad];
	if (p->sent_mode == PAD_MODE_PC) {
		*lit = 0;
		return;
	}
	if (program_pad_toggle(pad)) return;
	if (p->sent_mode == PAD_MODE_NOTE) push_note(p->sent_channel, p->sent_number, 0, 127);
	else push_cc(p->sent_channel, p->sent_number, 0);
	*lit = 0;
}

void pads_init(void)
{
	last_generation = adc_generation;
	for (uint8_t p = 0; p < PADS_NUM; p++) {
		envelope[p].active = 0;
		envelope[p].attack_count = 0;
		envelope[p].release_count = RELEASE_HOLD;
		envelope[p].peak = 0;
		for (uint8_t m = 0; m < 3; m++)
			for (uint8_t b = 0; b < 2; b++) output_active[m][b][p] = 0;
	}
}

void pads_process(void)
{
	if (last_generation == adc_generation) return;
	last_generation = adc_generation;
	for (uint8_t i = 0; i < PADS_NUM; i++) {
		pad_envelope_t *p = &envelope[i];
		uint16_t value = adc_raw[ADC_NUM_CHANNELS + i];
		if (!p->active) {
			if (value > ATTACK_THRESHOLD) {
				if (p->attack_count == 0 || value > p->peak) p->peak = value;
				if (++p->attack_count >= ATTACK_SAMPLES) {
					if (p->peak > PEAK_LIMIT) p->peak = PEAK_LIMIT;
					uint16_t scaled = (uint16_t)(((uint32_t)(p->peak - ATTACK_THRESHOLD) * 127u) / 0x220u);
					uint8_t velocity = scaled == 0 ? 1 : (scaled > 127 ? 127 : (uint8_t)scaled);
					/* Pads get their own curve: a piezo's response is
					 * nothing like a key's two-contact timing, so one
					 * setting for both would suit neither. */
					velocity = velocity_apply(program_pad_curve(), velocity,
					                          program_pad_fixed_velocity());
					p->active = 1;
					p->release_count = RELEASE_HOLD;
					pad_hit(i, velocity);
				}
			} else {
				p->attack_count = 0;
				p->peak = 0;
			}
		} else if (value < RELEASE_THRESHOLD) {
			if (p->release_count == 0) {
				p->active = 0;
				p->attack_count = 0;
				p->peak = 0;
				pad_release(i);
			} else {
				p->release_count--;
			}
		} else {
			p->release_count = RELEASE_HOLD;
		}
	}
}

void pads_all_off(void)
{
	for (uint8_t pad = 0; pad < PADS_NUM; pad++) {
		for (uint8_t bank = 0; bank < 2; bank++) {
			if (output_active[PAD_MODE_NOTE - 1][bank][pad]) {
				uint8_t old = program_pad_bank();
				program_set_pad_bank(bank);
				push_note(program_pad_channel(), program_pad_note(pad), 0, 127);
				program_set_pad_bank(old);
			}
			if (output_active[PAD_MODE_CC - 1][bank][pad]) {
				uint8_t old = program_pad_bank();
				program_set_pad_bank(bank);
				push_cc(program_pad_channel(), program_pad_cc(pad), 0);
				program_set_pad_bank(old);
			}
			for (uint8_t mode = 0; mode < 3; mode++) output_active[mode][bank][pad] = 0;
		}
		envelope[pad].active = 0;
		envelope[pad].attack_count = 0;
	}
}

uint8_t pads_active_mask(void)
{
	uint8_t mask = 0;
	uint8_t mode = program_pad_mode() - 1;
	uint8_t bank = program_pad_bank();
	for (uint8_t i = 0; i < PADS_NUM; i++) if (output_active[mode][bank][i]) mask |= (uint8_t)(1u << i);
	return mask;
}
