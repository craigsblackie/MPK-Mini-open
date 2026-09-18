/*
 * Original MPK mini mk1 LED behavior.
 *
 * The schematic shows two 74HC164 shift registers with a shared clock:
 * PB3=S_CLK, PB4=S_DAT0 (eight pad LEDs), PB5=S_DAT1 (eight status
 * LEDs).  FUN_080067f0 in the verified stock image shifts both bytes
 * together, MSB first, pulsing PB3 low/high for each bit.  Since a
 * 74HC164 presents the last bit at QA, that makes ordinary bit N map to
 * LED N in the schematic.
 *
 * The second byte is reconstructed from every stock writer:
 *   0 bank A, 1 bank B, 2 CC mode, 3 Program Change mode,
 *   4 Arp On/Off, 5 Tap Tempo beat, 6 Octave Down, 7 Octave Up.
 * The first byte mirrors each pad's active state, including the stock
 * behavior of leaving a pad lit when configured as a toggle.
 */
#include "leds.h"
#include "stm32f102.h"
#include "pads.h"
#include "program.h"
#include "arp.h"
#include "systick.h"
#include "transport.h"

#define LED_CLK  (1u << 3)
#define LED_PAD_DATA (1u << 4)
#define LED_STATUS_DATA (1u << 5)

#define STATUS_BANK_A    (1u << 0)
#define STATUS_BANK_B    (1u << 1)
#define STATUS_CC        (1u << 2)
#define STATUS_PC        (1u << 3)
#define STATUS_ARP       (1u << 4)
#define STATUS_TAP       (1u << 5)
#define STATUS_OCT_DOWN  (1u << 6)
#define STATUS_OCT_UP    (1u << 7)

static uint8_t last_pads;
static uint8_t last_status;
static uint8_t have_last;

static void shift_bytes(uint8_t pads, uint8_t status)
{
	for (uint8_t mask = 0x80u; mask != 0; mask >>= 1) {
		if (pads & mask) {
			GPIOB->BSRR = LED_PAD_DATA;
		} else {
			GPIOB->BRR = LED_PAD_DATA;
		}
		if (status & mask) {
			GPIOB->BSRR = LED_STATUS_DATA;
		} else {
			GPIOB->BRR = LED_STATUS_DATA;
		}
		GPIOB->BRR = LED_CLK;
		GPIOB->BSRR = LED_CLK;
	}
}

void leds_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPBEN;

	/* Release PB3/PB4 from JTAG while retaining the connected SWD
	 * debugger on PA13/PA14. */
	AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG_MASK) |
	             AFIO_MAPR_SWJ_CFG_SWD_ONLY;

	/* PB3..PB5: 2 MHz general-purpose push-pull outputs, exactly the
	 * stock GPIO configuration (CRL nibble value 0x2). */
	GPIOB->CRL = (GPIOB->CRL & ~((0xFu << 12) | (0xFu << 16) | (0xFu << 20))) |
	             (0x2u << 12) | (0x2u << 16) | (0x2u << 20);
	GPIOB->BRR = LED_CLK | LED_PAD_DATA | LED_STATUS_DATA;

	have_last = 0;
	last_pads = 0;
	last_status = 0;
	shift_bytes(0, 0);
}

void leds_process(void)
{
	uint8_t pads = pads_active_mask();
	uint8_t status = program_pad_bank() ? STATUS_BANK_B : STATUS_BANK_A;
	uint8_t mode = program_pad_mode();
	uint8_t octave = program_octave();

	if (mode == PAD_MODE_CC) {
		status |= STATUS_CC;
	} else if (mode == PAD_MODE_PC) {
		status |= STATUS_PC;
	}

	if (program_arp_enabled() || transport_arp_held()) {
		status |= STATUS_ARP;
	}
	uint32_t beat_ms = arp_beat_interval_ms();
	if (beat_ms != 0 && (systick_millis() % beat_ms) < (beat_ms / 2u)) {
		status |= STATUS_TAP;
	}

	if (octave < 4) {
		status |= STATUS_OCT_DOWN;
	} else if (octave > 4) {
		status |= STATUS_OCT_UP;
	}

	if (!have_last || pads != last_pads || status != last_status) {
		shift_bytes(pads, status);
		last_pads = pads;
		last_status = status;
		have_last = 1;
	}
}

static void factory_delay(void)
{
	/* This is the delay-loop shape and bounds used by stock. */
	for (volatile uint8_t outer = 0; outer < 100; outer++)
		for (volatile uint16_t inner = 0; inner < 30000; inner++) {}
}

void leds_factory_reset_blink(void)
{
	for (uint8_t cycle = 0; cycle < 3; cycle++) {
		shift_bytes(0xff, 0xff);
		factory_delay();
		shift_bytes(0, 0);
		factory_delay();
	}
	/* Force leds_process() to publish the normal panel state next. */
	have_last = 0;
}

static void delay_ms(uint32_t duration)
{
	uint32_t start = systick_millis();
	while ((uint32_t)(systick_millis() - start) < duration) {}
}

static void show_chase_light(uint8_t position)
{
	if (position < 8) {
		shift_bytes((uint8_t)(1u << position), 0);
	} else {
		/* Return across the panel in the opposite bit direction so the
		 * 16 steps read as one continuous loop rather than two sweeps. */
		shift_bytes(0, (uint8_t)(0x80u >> (position - 8)));
	}
}

void leds_boot_show(void)
{
	/* Deliberately non-stock signature: one measured lap followed by an
	 * accelerating lap, then a short all-light arrival flash. */
	static const uint8_t on_ms[2] = {50, 28};
	static const uint8_t gap_ms[2] = {10, 6};
	for (uint8_t lap = 0; lap < 2; lap++) {
		for (uint8_t position = 0; position < 16; position++) {
			show_chase_light(position);
			delay_ms(on_ms[lap]);
			shift_bytes(0, 0);
			delay_ms(gap_ms[lap]);
		}
	}
	shift_bytes(0xff, 0xff);
	delay_ms(100);
	shift_bytes(0, 0);
	delay_ms(30);
	have_last = 0;
}
