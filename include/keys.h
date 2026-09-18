#ifndef KEYS_H
#define KEYS_H

#include <stdint.h>

void keys_init(void);

/* Call once per main loop iteration, after matrix_scan(). Detects
 * transitions in matrix_state[] and pushes Note On/Off USB-MIDI
 * events to the MIDI ring buffer. */
void keys_process(void);
void keys_all_off(void);

#endif /* KEYS_H */
