#ifndef SYSEX_BRIDGE_H
#define SYSEX_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Request/response SysEx over the existing UART link to the keyboard.
 *
 * The keyboard answers an editor command with a SysEx of its own, but
 * nothing in raw MIDI correlates a reply with its request. This serialises
 * them instead: one request may be in flight at a time, and the UART task
 * hands each complete inbound SysEx to sysex_bridge_offer(), which wakes
 * the waiting caller if the message looks like the reply it asked for.
 *
 * Unmatched SysEx still goes to the BLE side as usual -- the editor taps
 * the stream, it does not consume it.
 */
#define SYSEX_BRIDGE_MAX 160u

void sysex_bridge_init(void);

/* Offer a complete inbound SysEx (F0..F7) to whoever is waiting.
 * Returns true if it was consumed as a reply. Called from the UART task. */
bool sysex_bridge_offer(const uint8_t *message, size_t len);

/*
 * Send `request` and wait for a reply whose command byte (offset 4)
 * matches `expect_cmd`. Returns the reply length, or 0 on timeout.
 * Thread safe; concurrent callers are serialised.
 */
size_t sysex_bridge_request(const uint8_t *request, size_t request_len,
                            uint8_t expect_cmd, uint8_t *reply, size_t reply_cap,
                            uint32_t timeout_ms);

/* True once any reply has ever been seen -- i.e. the UART link is real
 * and the keyboard is running firmware that answers. */
bool sysex_bridge_keyboard_seen(void);

#endif /* SYSEX_BRIDGE_H */
