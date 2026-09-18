#ifndef MIDI_RING_H
#define MIDI_RING_H

#include <stdint.h>
#include <stddef.h>

/*
 * MIDI TX ring buffer. Matches the original firmware's confirmed
 * design (240-byte circular buffer of USB-MIDI event bytes -- see
 * FIRMWARE_ANALYSIS.md), reimplemented as a standard circular buffer
 * rather than copied from the disassembly.
 *
 * This is THE mirror tap point for the ESP32-C3 BLE MIDI project:
 * call midi_ring_hook() (or add your own call alongside
 * midi_ring_push()) to also transmit every outgoing MIDI event over
 * USART1/PA9.
 */
#define MIDI_RING_SIZE 240

void midi_ring_init(void);

/* Returns 1 if the full write succeeded, 0 if the buffer didn't have
 * room (original firmware silently drops bytes that don't fit --
 * matches that behavior; TODO: decide if a replacement should do
 * something better here). */
int midi_ring_push(const uint8_t *data, size_t len);

/* Drains up to max_len bytes into dst, returns the number actually
 * written. */
size_t midi_ring_drain(uint8_t *dst, size_t max_len);

size_t midi_ring_count(void);

#endif /* MIDI_RING_H */
