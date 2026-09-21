/*
 * AD07 open-source replacement firmware -- main entry point.
 *
 * Implements the stock controller surface: velocity keys and pads,
 * knobs, pad banks/modes, transport/modifier buttons, six-mode arp,
 * persistent programs, editor SysEx, USB MIDI, and both LED registers.
 *
 * This is the field-updatable image. It lives in the application slot
 * (otamap.h), above everything the stock AKAI updater inspects, and is
 * reached through the resident loader rather than directly from the
 * stock updater's jump. It can rewrite its own slot over the 'f' SysEx
 * command, which is what the ESP32's web editor drives.
 */
#include "stm32f102.h"
#include "clock.h"
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
#include "ota.h"

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
	ota_init();

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
		ota_process();

		if (midi_ring_count() > 0 && usb_midi_ready()) {
			uint8_t pkt[64];
			size_t n = midi_ring_drain(pkt, sizeof pkt);
			usb_midi_send(pkt, n);
		}
	}
}
