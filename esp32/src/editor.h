#ifndef EDITOR_H
#define EDITOR_H

#include <stdbool.h>

/*
 * On-demand WiFi editor portal.
 *
 * The ESP32-C3 already talks the keyboard's editor SysEx, so it can host
 * the editor itself: it raises a SoftAP, serves a single-page app, and
 * translates that page's requests into the same SysEx the discontinued
 * AKAI editor used. No driver, no host software, nothing to install.
 *
 * It is off by default and toggled with a two-second PROGRAM hold or the
 * ESP32's BOOT button, for two reasons:
 * the radio would otherwise be sharing the antenna with BLE for no
 * benefit while playing, and a permanently-on AP roughly doubles the
 * bridge's current draw on a supply taken from the keyboard's USB rail.
 */
void editor_init(void);

/* Toggle the portal. Safe to call from a task context, not an ISR. */
void editor_toggle(void);
bool editor_active(void);

#endif /* EDITOR_H */
