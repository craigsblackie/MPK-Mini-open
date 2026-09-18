#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>

/*
 * Matrix column 7 button cluster: Arp, Tap Tempo, Sustain, Program,
 * and Octave Down/Up. See transport.c for the mappings.
 */
void transport_init(void);

/* status_byte: matrix_state[7] (confirmed source -- see transport.c). */
void transport_process(uint8_t status_byte);
uint8_t transport_program_held(void);
uint8_t transport_arp_held(void);
void transport_mark_arp_setting_used(void);

#endif /* TRANSPORT_H */
