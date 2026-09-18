#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stddef.h>

void usb_init(void);
void usb_poll(void);

/* Nonzero when EP1 IN can accept a new packet immediately. Callers
 * must not remove queued MIDI data until this reports ready. */
int usb_midi_ready(void);

/* Send one USB-MIDI packet (up to 64 bytes, already formatted as
 * 4-byte USB-MIDI events) out endpoint 0x81 IN. This is the low-level
 * transmit call -- see midi_ring.h for the buffer that feeds it, and
 * FIRMWARE_ANALYSIS.md's "complete USB MIDI TX pipeline" section for
 * how the original firmware structures this same handoff. */
void usb_midi_send(const uint8_t *data, size_t len);

/* Called by usb.c whenever a USB-MIDI packet arrives on EP1 OUT
 * (incoming MIDI from the host, e.g. a DAW driving this device's pads
 * as a control surface). Default implementation (in usb.c) does
 * nothing -- override by defining your own non-weak
 * usb_midi_on_receive() elsewhere, e.g. to forward events somewhere
 * useful. len is a multiple of 4 (one or more USB-MIDI events). */
void usb_midi_on_receive(const uint8_t *data, size_t len);

#endif /* USB_H */
