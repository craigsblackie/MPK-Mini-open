#pragma once
#include <stdint.h>
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
#define BLE_UUID128_INIT(...) { .u = {2}, .value = {__VA_ARGS__} }
