/*
 * Matrix column 8 button cluster: pad output mode select (Note/CC/PC).
 *
 * Originally implemented as "octave up/down" (FUN_080044fc's toggle-
 * between-two-values pattern). That guess is now resolved properly:
 * reading the function in full and tracing what its state variables
 * actually feed (via `get_xrefs_to`) shows `DAT_080045c0+2` is read by
 * `FUN_08003ab8` (the pad velocity handler, as `*DAT_08003e18` -- the
 * same SRAM address, 0x20000023, under a different literal-pool
 * alias) -- the shared runtime variable this project's `pads.c` and
 * `program.c` already call the "pad output mode" (Note=1/CC=2/PC=3),
 * previously only confirmed from the consumer side. This is the other
 * half: **these are the buttons that select it.**
 *
 * Confirmed logic, reproduced from the decompiled source:
 *  - **Bit 2**: pressing it toggles pad mode between CC and Note --
 *    if currently CC, back to Note; otherwise, to CC.
 *  - **Bit 3**: pressing it toggles pad mode between Program Change
 *    and Note, the same way.
 *  - **Bits 0/1** select pad bank A/B.  This is now cross-confirmed by
 *    the LED builder: the exact byte written here is read by
 *    FUN_08004c90 to choose status LED bit 0 or bit 1, while the pad
 *    handler uses it as the second index in each pad's paired Note,
 *    PC, and CC values.
 */
#include "buttons.h"
#include "program.h"

#define BIT_IDLE (1u << 0)
#define BIT_ALT (1u << 1)
#define BIT_CC_MODE (1u << 2)
#define BIT_PC_MODE (1u << 3)

static uint8_t previous_status;

void buttons_init(void)
{
	previous_status = 0;
	program_set_pad_bank(0);
}

void buttons_process(uint8_t status_byte)
{
	uint8_t changed = status_byte ^ previous_status;
	previous_status = status_byte;

	if (changed & BIT_ALT) {
		if (status_byte & BIT_ALT) {
			program_set_pad_bank(1);
		}
	}

	if (changed & BIT_IDLE) {
		if (status_byte & BIT_IDLE) {
			program_set_pad_bank(0);
		}
	}

	if ((changed & BIT_CC_MODE) && (status_byte & BIT_CC_MODE)) {
		if (program_pad_mode() == PAD_MODE_CC) {
			program_set_pad_mode(PAD_MODE_NOTE);
		} else {
			program_set_pad_mode(PAD_MODE_CC);
		}
	}

	if ((changed & BIT_PC_MODE) && (status_byte & BIT_PC_MODE)) {
		if (program_pad_mode() == PAD_MODE_PC) {
			program_set_pad_mode(PAD_MODE_NOTE);
		} else {
			program_set_pad_mode(PAD_MODE_PC);
		}
	}
}
