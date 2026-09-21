#pragma once
#include <stdint.h>
#include "ble_hs.h"
#define BLE_GAP_EVENT_CONNECT 0
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_ADV_COMPLETE 2
#define BLE_GAP_EVENT_SUBSCRIBE 3
#define BLE_GAP_EVENT_MTU 4
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
struct ble_gap_upd_params { uint16_t itvl_min, itvl_max, latency, supervision_timeout, min_ce_len, max_ce_len; };
struct ble_gap_adv_params { int conn_mode, disc_mode; };
struct ble_gap_event { int type; union {
  struct { int status; uint16_t conn_handle; } connect;
  struct { uint16_t attr_handle; int cur_notify; } subscribe;
  struct { uint16_t value; } mtu; }; };
static inline int ble_gap_adv_set_fields(const struct ble_hs_adv_fields*f){(void)f;return 0;}
static inline int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields*f){(void)f;return 0;}
static inline int ble_gap_adv_start(uint8_t a,void*b,int32_t c,const struct ble_gap_adv_params*d,int(*e)(struct ble_gap_event*,void*),void*f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
static inline int ble_gap_update_params(uint16_t h,const struct ble_gap_upd_params*p){(void)h;(void)p;return 0;}
