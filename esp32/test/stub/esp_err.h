#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERROR_CHECK(x) (void)(x)

static inline const char *esp_err_to_name(esp_err_t e){(void)e;return "ESP_FAIL";}
