/*
 * Resident image: loader plus recovery application.
 *
 * This is what the stock AKAI updater's checksum covers and what its
 * jump lands on, so it is installed once over SWD and never rewritten
 * in the field (otamap.h explains why that matters). Everything here is
 * therefore deliberately small and self-contained -- including its own
 * fifteen-line copy of the LED shift-out, rather than a dependency on
 * leds.c, which exists to reproduce the stock panel behavior and drags
 * in the program store, pads and arp to do it.
 *
 * On reset it decides between the application slot and recovery, and if
 * it stays in recovery it brings up just enough of the machine -- the
 * 48 MHz clock, USB MIDI, the ESP32 UART and the SysEx layer -- to be
 * found and reflashed. A unit in recovery plays nothing and lights a
 * slow sweep across the pad LEDs, so the state is unmistakable.
 *
 * It also answers the two-second PROGRAM hold that toggles the ESP32's
 * WiFi editor. That is not decoration: recovery is exactly when the
 * editor matters most, and a keyboard in a closed case has no other way
 * to reach it -- the alternative is the ESP32's own BOOT button, which
 * is inside. There is no matrix scanner here, so the one panel column
 * the gesture needs is polled directly through the loader's own read.
 */
#include "stm32f102.h"
#include "clock.h"
#include "systick.h"
#include "loader.h"
#include "app_image.h"
#include "midi_ring.h"
#include "midi_uart.h"
#include "sysex.h"
#include "usb.h"
#include "ota.h"
#include "otamap.h"
#include "program_hold.h"

#define LED_CLK         (1u << 3)
#define LED_PAD_DATA    (1u << 4)
#define LED_STATUS_DATA (1u << 5)

/* See leds.c: two 74HC164s on a shared clock, PB3 clock, PB4 pad data,
 * PB5 status data, MSB first. */
static void leds_init_minimal(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPBEN;
	AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG_MASK) |
	             AFIO_MAPR_SWJ_CFG_SWD_ONLY;
	GPIOB->CRL = (GPIOB->CRL & ~0x00fff000u) | 0x00222000u;
}

static void leds_write(uint8_t pads, uint8_t status)
{
	for (uint8_t mask = 0x80u; mask != 0; mask >>= 1) {
		if (pads & mask) GPIOB->BSRR = LED_PAD_DATA;
		else GPIOB->BRR = LED_PAD_DATA;
		if (status & mask) GPIOB->BSRR = LED_STATUS_DATA;
		else GPIOB->BRR = LED_STATUS_DATA;
		GPIOB->BRR = LED_CLK;
		GPIOB->BSRR = LED_CLK;
	}
}

/*
 * Idle: a single light sweeping back and forth across the eight pads,
 * which no normal mode of the instrument produces. While an upload is
 * running the same eight LEDs become a progress bar, so the transfer is
 * visible on the unit itself and not only in the browser.
 */
#define SWEEP_INTERVAL_MS 90u

static void recovery_leds(void)
{
	static uint32_t last_step;
	static uint8_t position;
	static uint8_t rising = 1;
	static uint8_t last_pads = 0xffu;
	/* Position walks 0..7 and back. Both ends turn around without
	 * stepping past, so the shift below can never run off the byte. */

	uint32_t now = systick_millis();
	uint8_t pads;

	uint32_t received = ota_received();
	if (received != 0u) {
		/* Eight segments of the image, filling left to right. */
		uint32_t lit = (received * 8u) / APP_SLOT_SIZE;
		if (lit > 8u) lit = 8u;
		pads = (uint8_t)((lit >= 8u) ? 0xffu : ((1u << lit) - 1u));
	} else {
		if (now - last_step < SWEEP_INTERVAL_MS) return;
		last_step = now;
		if (rising) {
			if (position == 7u) { rising = 0; position = 6u; }
			else position++;
		} else {
			if (position == 0u) { rising = 1; position = 1u; }
			else position--;
		}
		pads = (uint8_t)(1u << position);
	}

	if (pads != last_pads) {
		leds_write(pads, 0);
		last_pads = pads;
	}
}

/* The panel column is a shared resource with the LED shift-out only in
 * the sense that both touch GPIOB; the LEDs are on PB3-5 (CRL) and the
 * matrix rows on PB8-15 (CRH), so they do not collide. Column 7 is
 * selected once at startup and left selected, so a poll is one register
 * read -- nothing here may block, because a firmware image is arriving
 * on a UART with no receive FIFO while it runs. */
#define PANEL_POLL_MS 5u
#define PANEL_PROGRAM (1u << 3)

static void recovery_panel(void)
{
	static program_hold_t hold;
	static uint32_t last_poll;
	static uint8_t initialised;
	static uint8_t stable, last_sample;

	if (!initialised) {
		program_hold_init(&hold);
		initialised = 1;
	}

	uint32_t now = systick_millis();
	if (now - last_poll < PANEL_POLL_MS) return;
	last_poll = now;

	/* Two consecutive agreeing reads, the same debounce depth the
	 * matrix scanner uses for the panel columns. */
	uint8_t sample = (uint8_t)(loader_panel_rows() & PANEL_PROGRAM);
	if (sample != last_sample) {
		last_sample = sample;
		stable = 0;
		return;
	}
	if (stable < 2u) {
		stable++;
		if (stable < 2u) return;
	}

	if (program_hold_update(&hold, sample != 0u, now)) midi_uart_editor_toggle();
}

static void recovery_main(void)
{
	clock_init();
	systick_init();
	leds_init_minimal();
	leds_write(0, 0);
	midi_ring_init();
	sysex_init();
	midi_uart_init();
	usb_init();
	ota_init();
	loader_panel_init();
	loader_panel_select();

	while (1) {
		midi_uart_process();
		usb_poll();
		ota_process();
		recovery_panel();
		recovery_leds();

		if (midi_ring_count() > 0 && usb_midi_ready()) {
			uint8_t pkt[64];
			size_t n = midi_ring_drain(pkt, sizeof pkt);
			usb_midi_send(pkt, n);
		}
	}
}

int main(void)
{
	if (!loader_recovery_forced() && app_image_valid()) {
		loader_jump_to_app();
	}
	recovery_main();
	return 0;
}
