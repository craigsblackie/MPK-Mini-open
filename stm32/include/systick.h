#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>

/*
 * SysTick-based millisecond timebase.
 *
 * Standard Cortex-M3 SysTick usage (not derived from the original
 * firmware's disassembly -- a later pass found the original *does*
 * configure SysTick hardware (LOAD/TICKINT/ENABLE all set), but its
 * SysTick_Handler is a no-op stub, `bx lr`; whatever the original uses
 * it for, it isn't interrupt-driven timekeeping -- see
 * FIRMWARE_ANALYSIS.md's dual-switch key velocity follow-up section
 * for the full story of tracking this down). This gives this
 * replacement firmware a real wall-clock reference, used by
 * stuck_note.c's timeout and arp.c's step rate.
 */
void systick_init(void);

/* Milliseconds since systick_init(), wrapping at ~49.7 days (uint32_t
 * overflow) -- not handled specially, same as most embedded millis()
 * implementations. */
uint32_t systick_millis(void);

#endif /* SYSTICK_H */
