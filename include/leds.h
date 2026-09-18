#ifndef LEDS_H
#define LEDS_H

/* Two 74HC164s drive the eight pad backlights and eight status LEDs.
 * leds_process() derives both bytes from the same runtime states used
 * by the stock firmware and only shifts them when something changes. */
void leds_init(void);
void leds_process(void);
void leds_factory_reset_blink(void);
void leds_boot_show(void);

#endif /* LEDS_H */
