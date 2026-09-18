#ifndef ADC_H
#define ADC_H

#include <stdint.h>

#define ADC_NUM_CHANNELS 8     /* knobs -- channels 0-7 (PA0-7) */
#define ADC_NUM_PAD_CHANNELS 8 /* pads -- channels 8-15 (PB0-1, PC0-5) */
#define ADC_TOTAL_CHANNELS (ADC_NUM_CHANNELS + ADC_NUM_PAD_CHANNELS)

/* Raw 12-bit ADC readings, continuously updated by DMA in the
 * background -- no polling/blocking needed to read these.
 * Index 0..ADC_NUM_CHANNELS-1: knobs. Index ADC_NUM_CHANNELS..
 * ADC_TOTAL_CHANNELS-1: pads (see pads.h). */
extern volatile uint16_t adc_raw[ADC_TOTAL_CHANNELS];
extern volatile uint32_t adc_generation;

void adc_init(void);
void adc_process(void);

#endif /* ADC_H */
