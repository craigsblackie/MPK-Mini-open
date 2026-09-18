#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>

/*
 * Matrix column 8 button cluster: pad Bank A/B and output mode select
 * (Note/CC/PC).
 * See buttons.c for the confirmed derivation.
 */
void buttons_init(void);

/* status_byte: matrix_state[8] (confirmed source). */
void buttons_process(uint8_t status_byte);

#endif /* BUTTONS_H */
