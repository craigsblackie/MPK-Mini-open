#pragma once
#include <stdint.h>
#include <stddef.h>
#define BLE_HS_ENOMEM 1
#define BLE_HS_FOREVER INT32_MAX
#define BLE_SM_IO_CAP_NO_IO 3
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
struct os_mbuf { uint8_t *data; uint16_t len; };
#define OS_MBUF_PKTLEN(om) ((om)->len)
struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len);
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *buf, uint16_t len, uint16_t *out);
static inline int ble_hs_id_infer_auto(int p, uint8_t *t){(void)p;*t=0;return 0;}
struct ble_hs_cfg_t { void (*reset_cb)(int); void (*sync_cb)(void); int sm_io_cap, sm_bonding, sm_sc, sm_our_key_dist, sm_their_key_dist; };
extern struct ble_hs_cfg_t ble_hs_cfg;
struct ble_hs_adv_fields { uint8_t flags; const void *uuids128; int num_uuids128, uuids128_is_complete; uint8_t *name; uint8_t name_len; int name_is_complete; };
#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
