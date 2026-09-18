#ifndef MATRIX_H
#define MATRIX_H

#include <stdint.h>

#define MATRIX_COLS 9
#define MATRIX_ROWS 8

/*
 * Debounced electrical state for each column (bit N = row N, low when
 * active). Key processing consumes columns 0-6; panel controls use 7-8.
 */
extern uint8_t matrix_state[MATRIX_COLS];

void matrix_init(void);
void matrix_scan(void);

#endif /* MATRIX_H */
