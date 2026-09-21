#ifndef OTA_H
#define OTA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Receiving side of the 'f' firmware-transfer command (otaproto.h).
 *
 * Writes into the application slot described by otamap.h. Compiled into
 * both the application and the resident recovery image, so an update can
 * be pushed whether the unit is running normally or has fallen back to
 * recovery after an interrupted one.
 *
 * The sequence is BEGIN, then DATA chunks strictly in order from offset
 * zero, then COMMIT. BEGIN erases the trailer, which is what marks the
 * slot bootable, so from that moment until COMMIT succeeds the loader
 * will choose recovery. There is no window in which a partly written
 * image can be jumped into.
 */

void ota_init(void);

/*
 * Handle one complete 'f' SysEx message, F0 through F7, and reply
 * through ota_emit(). `id` is the editor id byte to echo back.
 *
 * The framing lives here rather than in sysex.c so that both this
 * firmware's host tests and the ESP32 bridge's can drive the real
 * message layer instead of a reimplementation that could agree with
 * itself while disagreeing with the device.
 */
void ota_handle_message(const uint8_t *message, size_t len, uint8_t id);

/* Supplied by the caller -- sysex.c on the target, the test harness on a
 * host. Sends one complete SysEx message to whoever asked. */
void ota_emit(const uint8_t *message, size_t len);

/* The individual sub-commands, exposed so a test can reach a single step
 * without going through the wire. Each returns an OTA_STATUS_* code, and
 * `payload`/`len` are the bytes between the sub-command and the F7. */
uint8_t ota_begin(const uint8_t *payload, size_t len);
uint8_t ota_data(const uint8_t *payload, size_t len, uint32_t *offset_out);
uint8_t ota_commit(const uint8_t *payload, size_t len);
uint8_t ota_abort(void);

/*
 * Read part of the installed application back out. Writes the raw bytes
 * into `out` (which must hold OTA_CHUNK_BYTES) and reports how many
 * through `count_out`, so a copy of what is on the device can be
 * downloaded without a debugger. Refuses anything outside the installed
 * image's own length, so a download cannot wander into the erased
 * remainder of the slot or past the end of flash.
 */
uint8_t ota_read(const uint8_t *payload, size_t len, uint8_t *out,
                 uint32_t *offset_out, uint32_t *count_out);

/* How many bytes of the current transfer have been written. */
uint32_t ota_received(void);

/* True once COMMIT has accepted an image and a reboot is pending. */
bool ota_reboot_pending(void);

/*
 * Called from the main loop. Reboots the unit a short time after a
 * successful COMMIT -- long enough for the reply to have drained out of
 * both the USB and UART queues, so the ESP32 learns the update was
 * accepted rather than watching the link go silent. Also abandons a
 * transfer that has stalled, so an interrupted upload does not leave the
 * slot locked against the next attempt.
 */
void ota_process(void);

#endif /* OTA_H */
