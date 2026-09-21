#pragma once
#include <stdint.h>
extern int64_t fake_us;
static inline int64_t esp_timer_get_time(void){ return fake_us; }
typedef struct { void (*callback)(void*); const char *name; } esp_timer_create_args_t;
typedef void* esp_timer_handle_t;
static inline int esp_timer_create(const esp_timer_create_args_t*a, esp_timer_handle_t*h){(void)a;(void)h;return 0;}
static inline int esp_timer_start_periodic(esp_timer_handle_t h, uint64_t p){(void)h;(void)p;return 0;}
