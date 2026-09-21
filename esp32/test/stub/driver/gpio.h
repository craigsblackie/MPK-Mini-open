#pragma once
#include <stdint.h>
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 0
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
static inline int gpio_config(const gpio_config_t*c){(void)c;return 0;}
static inline int gpio_set_level(int p,int l){(void)p;(void)l;return 0;}
/* Idle high: the BOOT button is active low and never pressed in tests. */
static inline int gpio_get_level(int p){(void)p;return 1;}
