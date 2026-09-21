/*
 * The PROGRAM hold that recovery uses to toggle the WiFi editor.
 *
 * This matters more than its size suggests. It lives in the resident
 * image, which is installed once over SWD and never rewritten in the
 * field, and it is the only way to reach the editor from a keyboard
 * whose case is shut. If it is wrong, the way to fix a unit that cannot
 * boot is itself unreachable.
 */
#include <stdio.h>
#include <stdlib.h>
#include "program_hold.h"

static int failures;

static void ok(const char *what, int cond)
{
	printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
	if (!cond) failures++;
}

/* Hold `pressed` for `ms` milliseconds in 5 ms polls, the rate
 * recovery_panel() uses, and count how many times it fires. */
static int run(program_hold_t *state, uint8_t pressed, uint32_t ms, uint32_t *now)
{
	int fired = 0;
	for (uint32_t t = 0; t < ms; t += 5) {
		fired += program_hold_update(state, pressed, *now);
		*now += 5;
	}
	return fired;
}

int main(void)
{
	program_hold_t s;
	uint32_t now = 1000;

	program_hold_init(&s);
	ok("short press does not toggle", run(&s, 1, 1500, &now) == 0);
	ok("release after a short press is quiet", run(&s, 0, 100, &now) == 0);

	program_hold_init(&s);
	ok("two-second hold toggles once", run(&s, 1, 2100, &now) == 1);
	ok("holding longer does not toggle again", run(&s, 1, 5000, &now) == 0);
	ok("releasing is quiet", run(&s, 0, 100, &now) == 0);
	ok("a second hold toggles again", run(&s, 1, 2100, &now) == 1);

	/* The boundary itself: it must fire at 2000 ms, not 1995 or 2005. */
	program_hold_init(&s);
	now = 0;
	int fired_before = 0;
	for (uint32_t t = 0; t < 2000; t += 5) {
		fired_before += program_hold_update(&s, 1, now);
		now += 5;
	}
	ok("silent for the first 2000 ms", fired_before == 0);
	ok("fires exactly at 2000 ms", program_hold_update(&s, 1, now) == 1);

	/* Tapping repeatedly must never accumulate toward a toggle. */
	program_hold_init(&s);
	int taps = 0;
	for (int i = 0; i < 40; i++) {
		taps += run(&s, 1, 500, &now);
		taps += run(&s, 0, 50, &now);
	}
	ok("forty taps never toggle", taps == 0);

	/* The millisecond counter wraps every ~49.7 days; a hold spanning
	 * the wrap must still behave, because the difference is unsigned. */
	program_hold_init(&s);
	now = 0xfffffc00u; /* 1024 ms before the wrap */
	ok("hold across the counter wrap toggles once", run(&s, 1, 2100, &now) == 1);

	printf("\n%s\n", failures ? "PROGRAM hold tests FAILED" : "all PROGRAM hold tests passed");
	return failures != 0;
}
