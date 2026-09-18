#ifndef SYSEX_H
#define SYSEX_H

#include <stdint.h>
#include <stddef.h>

/*
 * AKAI editor-software SysEx protocol -- receive support.
 *
 * Implements the wire framing and command dispatch for FUN_08002eac,
 * decoded in FIRMWARE_ANALYSIS.md's "Confirmed: SysEx editor-protocol
 * handler" section: reassembles incoming USB-MIDI SysEx packets
 * (CIN 0x4-0x7 events, handled by overriding usb_midi_on_receive() --
 * see usb.h) into complete messages, then dispatches 'b' (select
 * program), 'a' (write/receive a program dump), and 'c' (read/send a
 * program dump) using program.c's decoded record layout and wire
 * reorder table.
 *
 * Also implements 'd' status, 'j' bootstrap/reset, and the universal
 * identity request used by the editor. Stock's '`' service payload is
 * accepted and ignored; its handler has no observable durable effect.
 */
void sysex_init(void);

/* Feed a raw MIDI byte received from the ESP32 UART. Realtime messages
 * are routed to the arp; complete SysEx messages share the USB handler. */
void midi_input_byte(uint8_t byte);

#endif /* SYSEX_H */
