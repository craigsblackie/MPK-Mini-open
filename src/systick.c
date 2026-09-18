/*
 * SysTick-based millisecond timebase -- see systick.h.
 */
#include "systick.h"
#include "stm32f102.h"

#define SYSCLK_HZ 48000000u /* matches main.c's clock_init() -- HSE 8 MHz * PLL x6 */
#define TICKS_PER_MS (SYSCLK_HZ / 1000u)

static volatile uint32_t millis_count;

void systick_init(void)
{
	millis_count = 0;
	SysTick->LOAD = TICKS_PER_MS - 1u;
	SysTick->VAL = 0;
	SysTick->CTRL = SysTick_CTRL_CLKSOURCE | SysTick_CTRL_TICKINT | SysTick_CTRL_ENABLE;
}

uint32_t systick_millis(void)
{
	return millis_count;
}

void SysTick_Handler(void)
{
	millis_count++;
}
