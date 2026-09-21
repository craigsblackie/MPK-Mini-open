#ifndef CLOCK_H
#define CLOCK_H

/* Bring SYSCLK and the USB clock to 48 MHz from the board's 8 MHz
 * crystal. Must run before usb_init() and before systick_init(),
 * which both assume that rate. */
void clock_init(void);

#endif /* CLOCK_H */
