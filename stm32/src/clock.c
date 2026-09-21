/*
 * 8 MHz HSE -> PLL x6 -> 48 MHz system and USB clock.
 *
 * Split out of main.c so the resident loader/recovery image can
 * reach the same 48 MHz USB clock without pulling in the whole
 * application. Both images run this identically.
 */
#include "clock.h"
#include "stm32f102.h"

void clock_init(void)
{
	/* HSE (external 8 MHz crystal, confirmed from the Y1 marking in
	 * this project's board photos) -> PLL x6 -> 48 MHz SYSCLK/USBCLK.
	 * Standard STM32F1 startup sequence, the same shape for any
	 * board with this crystal -- not derived from the original
	 * firmware's disassembly (see FIRMWARE_ANALYSIS.md's FLASH_IF
	 * section, which found extensive but not-fully-traced RCC/flash
	 * activity early in the original's startup; this reimplements
	 * the standard procedure rather than that exact code). */

	RCC->CR |= RCC_CR_HSEON;
	while (!(RCC->CR & RCC_CR_HSERDY)) {
	}

	/* 2 wait states required above 48 MHz... actually required
	 * above 24 MHz per RM0008 Table 6; set before switching SYSCLK
	 * to the higher-speed PLL output. */
	FLASH_IF->ACR = FLASH_ACR_LATENCY_2;

	RCC->CFGR = (RCC->CFGR & ~((0x1Fu << 18) | (0x7u << 8) | (0x7u << 11))) |
	            (0x4u << 8) | (0x4u << 11) | /* APB1 /2, APB2 /2 */
	            RCC_CFGR_PLLSRC_HSE |
	            RCC_CFGR_PLLXTPRE_DIV1 | RCC_CFGR_PLLMUL6;

	RCC->CR |= RCC_CR_PLLON;
	while (!(RCC->CR & RCC_CR_PLLRDY)) {
	}

	RCC->CFGR = (RCC->CFGR & ~0x3u) | RCC_CFGR_SW_PLL;
	while ((RCC->CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL) {
	}

	/* USB clock: USBPRE bit (RCC_CFGR bit 22) = 0 selects PLL/1.5,
	 * which for a 48 MHz PLL gives 32 MHz -- wrong. USB full-speed
	 * needs exactly 48 MHz, so USBPRE must be 1 (PLL/1, no divide).
	 * This setting is now hardware-validated by successful full-speed
	 * enumeration, and the live CFGR matches the working original. */
	RCC->CFGR |= (1u << 22);
}
