#pragma once
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#define WIFI_INIT_CONFIG_DEFAULT() {0}
#define WIFI_MODE_AP 2
#define WIFI_IF_AP 1
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_PS_MIN_MODEM 1
typedef struct { int dummy; } wifi_init_config_t;
typedef struct { struct { uint8_t ssid[32]; uint8_t ssid_len; uint8_t password[64];
  uint8_t channel; uint8_t max_connection; int authmode; } ap; } wifi_config_t;
static inline esp_err_t esp_wifi_init(const wifi_init_config_t*c){(void)c;return 0;}
static inline esp_err_t esp_wifi_set_mode(int m){(void)m;return 0;}
static inline esp_err_t esp_wifi_set_config(int i,wifi_config_t*c){(void)i;(void)c;return 0;}
static inline esp_err_t esp_wifi_start(void){return 0;}
static inline esp_err_t esp_wifi_stop(void){return 0;}
static inline esp_err_t esp_wifi_set_ps(int p){(void)p;return 0;}
static inline esp_err_t esp_wifi_set_max_tx_power(int8_t p){(void)p;return 0;}
static inline esp_err_t esp_wifi_get_max_tx_power(int8_t *p){*p=44;return 0;}
