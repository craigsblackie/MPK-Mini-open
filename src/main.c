/*
 * AD07 open-source replacement firmware -- main entry point.
 *
 * Implements the stock controller surface: velocity keys and pads,
 * knobs, pad banks/modes, transport/modifier buttons, six-mode arp,
 * persistent programs, editor SysEx, USB MIDI, and both LED registers.
 * The original 8 KiB updater is retained ahead of this application.
 */
#include "stm32f102.h"
#include "systick.h"
#include "matrix.h"
#include "midi_ring.h"
#include "midi_uart.h"
#include "program.h"
#include "sysex.h"
#include "keys.h"
#include "adc.h"
#include "knobs.h"
#include "pads.h"
#include "buttons.h"
#include "transport.h"
#include "arp.h"
#include "usb.h"
#include "leds.h"

static void clock_init(void)
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

int main(void)
{
	clock_init();
	systick_init();
	matrix_init();
	leds_init();
	midi_ring_init();
	program_init();
	if (program_factory_reset_performed()) leds_factory_reset_blink();
	leds_boot_show();
	sysex_init();
	midi_uart_init();
	usb_init();
	adc_init();

	keys_init();
	knobs_init();
	pads_init();
	buttons_init();
	transport_init();
	arp_init();

	while (1) {
		adc_process();
		matrix_scan();
		keys_process();
		pads_process();
		buttons_process((uint8_t)~matrix_state[8]);
		transport_process((uint8_t)~matrix_state[7]);
		knobs_process();
		arp_process();
		leds_process();
		midi_uart_process();
		usb_poll();

		if (midi_ring_count() > 0 && usb_midi_ready()) {
			uint8_t pkt[64];
			size_t n = midi_ring_drain(pkt, sizeof pkt);
			usb_midi_send(pkt, n);
		}
	}
}
