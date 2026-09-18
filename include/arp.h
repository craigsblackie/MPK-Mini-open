#ifndef ARP_H
#define ARP_H

#include <stdint.h>

#define ARP_MAX_NOTES 25

/* Manual override, defaults to 1 -- actual enable is this ANDed with
 * the current program's own arp-enabled flag (program_arp_enabled()).
 * See arp.c's header comment. */
extern uint8_t arp_enabled;

void arp_init(void);
void arp_note_on(uint8_t note, uint8_t velocity);
void arp_note_off(uint8_t note);
void arp_process(void);
void arp_all_off(void);
void arp_midi_realtime(uint8_t byte);

/* Registers one tap. Once the configured 2-4 interval window is full,
 * the rolling average overrides the stored tempo. */
void arp_tap(void);

/* Current quarter-note period, including the temporary tap-tempo
 * override.  The original uses this period for the Tap Tempo LED. */
uint32_t arp_beat_interval_ms(void);

#endif /* ARP_H */
