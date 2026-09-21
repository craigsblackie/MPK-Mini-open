#include "program_hold.h"

void program_hold_init(program_hold_t *state)
{
	state->press_ms = 0;
	state->held = 0;
	state->fired = 0;
}

uint8_t program_hold_update(program_hold_t *state, uint8_t pressed, uint32_t now_ms)
{
	if (pressed && !state->held) {
		state->press_ms = now_ms;
		state->fired = 0;
	}
	state->held = pressed;

	if (!pressed) {
		state->fired = 0;
		return 0;
	}
	if (state->fired) return 0;
	/* Unsigned difference, so this is correct across the millisecond
	 * counter's wrap. */
	if ((uint32_t)(now_ms - state->press_ms) < PROGRAM_HOLD_MS) return 0;

	state->fired = 1;
	return 1;
}
