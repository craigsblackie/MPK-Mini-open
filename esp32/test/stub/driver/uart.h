#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define UART_NUM_1 1
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE (-1)
typedef struct { int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk; } uart_config_t;
static inline esp_err_t uart_driver_install(int p,int r,int t,int q,void*h,int f){(void)p;(void)r;(void)t;(void)q;(void)h;(void)f;return 0;}
static inline esp_err_t uart_param_config(int p,const uart_config_t*c){(void)p;(void)c;return 0;}
static inline esp_err_t uart_set_pin(int p,int tx,int rx,int rts,int cts){(void)p;(void)tx;(void)rx;(void)rts;(void)cts;return 0;}
static inline int uart_read_bytes(int p,void*b,uint32_t l,uint32_t t){(void)p;(void)b;(void)l;(void)t;return 0;}
void uart_write_bytes_capture(const uint8_t *d, size_t n);
static inline int uart_write_bytes(int p,const void*d,size_t n){(void)p; uart_write_bytes_capture((const uint8_t*)d,n); return (int)n;}
