/*
 * Knob -> MIDI Control Change.
 *
 * ADC values are the stock four-scan sums shifted to 10 bits. The
 * original deadband (7), inclusive endpoint scaling, reversible low/
 * high ranges, per-program CC assignments, and channel are retained.
 */
#include "knobs.h"
#include "adc.h"
#include "midi_ring.h"
#include "program.h"

#define RAW_DEADBAND 7

static uint8_t last_cc_value[ADC_NUM_CHANNELS];
static uint16_t last_raw[ADC_NUM_CHANNELS];
static uint32_t last_generation;

void knobs_init(void)
{
	for (int i = 0; i < ADC_NUM_CHANNELS; i++) {
		last_cc_value[i] = 0xFF; /* force a send on first real reading */
		last_raw[i] = 0xffff;
	}
	last_generation = adc_generation;
}

static void send_cc(uint8_t knob, uint8_t value)
{
	uint8_t cc_number = program_knob_cc(knob);
	if (cc_number == 0) {
		return; /* 0 = knob unassigned, per the original's confirmed
		         * "gate byte doubles as CC number" behavior. */
	}

	uint8_t event[4];
	event[0] = 0x0B; /* Cable 0, CIN: Control Change */
	event[1] = (uint8_t)(0xB0 | program_channel());
	event[2] = cc_number;
	event[3] = value;
	midi_ring_push(event, 4);

}

void knobs_process(void)
{
	if (last_generation == adc_generation) return;
	last_generation = adc_generation;

	for (int i = 0; i < ADC_NUM_CHANNELS; i++) {
		uint16_t averaged = adc_raw[i];
		if (averaged > 1015) averaged = 1015;
		uint8_t low = program_knob_low((uint8_t)i);
		uint8_t high = program_knob_high((uint8_t)i);
		uint8_t cc_value;
		if (high > low) {
			uint16_t v = (uint16_t)(low + ((uint32_t)averaged * (uint16_t)(high - low + 1u)) / 1015u);
			cc_value = v > high ? high : (uint8_t)v;
		} else {
			int16_t v = (int16_t)low - (int16_t)(((uint32_t)averaged * (uint16_t)(low - high + 1u)) / 1015u);
			cc_value = v < high ? high : (uint8_t)v;
		}
		uint16_t raw_diff = last_raw[i] == 0xffff ? 0xffff :
		                    (averaged > last_raw[i] ? averaged - last_raw[i] : last_raw[i] - averaged);
		if (last_cc_value[i] == 0xFF || (raw_diff >= RAW_DEADBAND && cc_value != last_cc_value[i])) {
			last_raw[i] = averaged;
			last_cc_value[i] = cc_value;
			send_cc((uint8_t)i, cc_value);
		}
	}
}
