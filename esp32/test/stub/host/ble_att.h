#pragma once
#include <stdint.h>
#define BLE_ATT_MTU_MAX 527
#define BLE_ATT_ERR_UNLIKELY 0x0e
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 0x0d
uint16_t ble_att_mtu(uint16_t conn);
