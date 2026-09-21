#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

#include "esp_http_server.h"

/*
 * Field updates for both processors, served from the editor portal.
 *
 * The bridge updates itself through ESP-IDF's ordinary two-slot OTA: a
 * new image is written to the inactive slot, booted on probation, and
 * reverted by the bootloader if it never reports itself healthy.
 *
 * The keyboard is updated over the same UART link the editor already
 * uses, with the 'f' SysEx command (stm32/include/otaproto.h). The
 * STM32 writes it into a slot that sits outside everything its stock
 * updater inspects, so an upload that is interrupted -- by a closed
 * browser tab, a lost WiFi link or a pulled USB cable -- costs nothing
 * worse than a keyboard that comes up in recovery and can be uploaded
 * to again.
 *
 * Named firmware_update rather than ota to keep it distinct from the
 * firmware's own ota.h, which this file's transfer loop talks to.
 */
void firmware_update_init(void);

/* Registers /api/firmware and its two upload endpoints. Called by
 * editor.c while the portal's HTTP server is starting. */
void firmware_update_register_routes(httpd_handle_t server);

/* Number of URI handlers the above installs, so editor.c can size the
 * server's handler table without the two drifting apart. */
#define FIRMWARE_UPDATE_ROUTE_COUNT 6

#endif /* FIRMWARE_UPDATE_H */
