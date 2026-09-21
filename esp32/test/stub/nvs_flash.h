#pragma once
#include "esp_err.h"
#define ESP_ERR_NVS_NO_FREE_PAGES 1
#define ESP_ERR_NVS_NEW_VERSION_FOUND 2
static inline esp_err_t nvs_flash_init(void){return 0;}
static inline esp_err_t nvs_flash_erase(void){return 0;}
