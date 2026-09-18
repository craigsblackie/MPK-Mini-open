#ifndef KNOBS_H
#define KNOBS_H

void knobs_init(void);

/* Call once per main loop iteration. Oversamples adc_raw[], and on a
 * meaningful change sends a Control Change message per knob. */
void knobs_process(void);

#endif /* KNOBS_H */
