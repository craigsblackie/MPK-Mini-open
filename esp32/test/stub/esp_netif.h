#pragma once
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
#define ESP_NETIF_OP_SET 1
#define ESP_NETIF_DOMAIN_NAME_SERVER 6
#define ESP_NETIF_CAPTIVEPORTAL_URI 114
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5001
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5002
static inline esp_err_t esp_netif_init(void){return 0;}
static inline esp_netif_t *esp_netif_create_default_wifi_ap(void){return 0;}
static inline esp_err_t esp_netif_dhcps_stop(esp_netif_t*n){(void)n;return 0;}
static inline esp_err_t esp_netif_dhcps_start(esp_netif_t*n){(void)n;return 0;}
static inline esp_err_t esp_netif_dhcps_option(esp_netif_t*n,int op,int id,void*v,uint32_t l){(void)n;(void)op;(void)id;(void)v;(void)l;return 0;}
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t*n,esp_netif_ip_info_t*i){(void)n;i->ip.addr=0x0104a8c0;return 0;}
