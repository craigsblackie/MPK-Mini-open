#pragma once
#include <stdint.h>
#include "ble_hs.h"
#include "ble_uuid.h"
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_READ 1
#define BLE_GATT_CHR_F_WRITE_NO_RSP 2
#define BLE_GATT_CHR_F_NOTIFY 4
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
struct ble_gatt_chr_def { const ble_uuid_t *uuid; int (*access_cb)(uint16_t,uint16_t,struct ble_gatt_access_ctxt*,void*); uint16_t *val_handle; int flags; };
struct ble_gatt_svc_def { int type; const ble_uuid_t *uuid; struct ble_gatt_chr_def *characteristics; };
int ble_gatts_notify_custom(uint16_t conn, uint16_t handle, struct os_mbuf *om);
static inline int ble_gatts_count_cfg(const struct ble_gatt_svc_def*s){(void)s;return 0;}
static inline int ble_gatts_add_svcs(const struct ble_gatt_svc_def*s){(void)s;return 0;}
