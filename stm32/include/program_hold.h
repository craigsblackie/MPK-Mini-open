#ifndef PROGRAM_HOLD_H
#define PROGRAM_HOLD_H

#include <stdint.h>

/*
 * The two-second PROGRAM hold that toggles the ESP32's WiFi editor.
 *
 * transport.c implements this for the application, where it has to
 * coexist with PROGRAM+key program selection. The recovery image has no
 * program selection and no matrix scanner, but it still needs the
 * gesture: a unit sitting in recovery inside a closed case has no other
 * way to reach the editor, and the editor is how you get an application
 * back into it. This is that logic on its own, so recovery can use it
 * without pulling in the rest of the panel handling -- and so it can be
 * tested rather than merely written.
 */

#define PROGRAM_HOLD_MS 2000u

typedef struct {
	uint32_t press_ms;
	uint8_t held;
	uint8_t fired;
} program_hold_t;

void program_hold_init(program_hold_t *state);

/*
 * Feed the current state of the PROGRAM button. Returns 1 exactly once
 * per press, at the moment the hold reaches PROGRAM_HOLD_MS, and 0 every
 * other time. Releasing and pressing again re-arms it.
 */
uint8_t program_hold_update(program_hold_t *state, uint8_t pressed, uint32_t now_ms);

#endif /* PROGRAM_HOLD_H */
