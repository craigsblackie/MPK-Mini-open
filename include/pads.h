#ifndef PADS_H
#define PADS_H

#include <stdint.h>

#define PADS_NUM 8

void pads_init(void);
void pads_process(void);
void pads_all_off(void);

/* Bit N is set while pad N's pressure envelope is active.  The stock
 * firmware uses the same active/latch state to drive the eight pad
 * backlights. */
uint8_t pads_active_mask(void);

#endif /* PADS_H */
