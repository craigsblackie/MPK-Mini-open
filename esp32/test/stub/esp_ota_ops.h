#pragma once
/* Just enough of the OTA API for firmware_update.c's bridge path. The
 * test drives the outcomes through the hooks below. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct { const char *label; int subtype; size_t size; } esp_partition_t;
typedef int esp_ota_handle_t;

typedef enum {
	ESP_OTA_IMG_NEW = 0,
	ESP_OTA_IMG_PENDING_VERIFY = 1,
	ESP_OTA_IMG_VALID = 2,
} esp_ota_img_states_t;

/* Set by the test to steer each call. */
extern const esp_partition_t *fake_next_partition;
extern esp_err_t fake_ota_begin_result;
extern esp_err_t fake_ota_end_result;
extern uint8_t fake_ota_image[262144];
extern size_t fake_ota_written;
extern int fake_ota_aborted;
extern int fake_ota_boot_set;
extern int fake_restart_count;

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *from);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *h);
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t len);
esp_err_t esp_ota_end(esp_ota_handle_t h);
esp_err_t esp_ota_abort(esp_ota_handle_t h);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
