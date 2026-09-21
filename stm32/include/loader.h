#ifndef LOADER_H
#define LOADER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The first thing the resident image does: decide whether to hand over
 * to the application slot, or stay here and run recovery.
 *
 * Nothing in this path writes flash and nothing here can be updated in
 * the field, so it is deliberately small and dependency-free -- it runs
 * on the reset-default 8 MHz HSI clock, before any peripheral except the
 * two GPIO ports the gesture needs.
 */

/* Whether there is anything to hand over to is app_image_valid(),
 * in app_image.h. */

/* Configure the two GPIO ports the panel column needs, exactly as
 * matrix_init() does. Safe to call more than once. */
void loader_panel_init(void);

/*
 * Read matrix column 7, the panel button column, as an active-high byte:
 * bit 0 arp, 1 tap tempo, 2 sustain, 3 PROGRAM, 4 octave down, 5 octave
 * up (transport.c). Recovery polls this for the PROGRAM hold, since it
 * has no matrix scanner of its own.
 */
uint8_t loader_read_panel(void);

/*
 * The same column, for continuous polling. loader_panel_select() drives
 * it once and leaves it driven; loader_panel_rows() then reads the rows
 * with no settling delay, because there is nothing to settle -- the
 * column never changes. Recovery uses this pair rather than
 * loader_read_panel(), whose settle is long enough to drop an incoming
 * MIDI byte on a UART with no receive FIFO.
 */
void loader_panel_select(void);
uint8_t loader_panel_rows(void);

/* True while TAP TEMPO is held. Holding it at power-on keeps the unit
 * in recovery even when the installed application is perfectly valid,
 * which is the way back from an image that passes its CRC but does not
 * work. The stock updater samples the same matrix column and claims
 * only the PROGRAM combination, so this gesture reaches us untouched
 * (FIRMWARE_ANALYSIS.md, "bootloader/DFU entry at boot"). */
bool loader_recovery_forced(void);

/* Hand over to the application slot. Does not return. */
void loader_jump_to_app(void);

#endif /* LOADER_H */
