#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#define HTTP_GET 1
#define HTTP_POST 2
#define HTTPD_DEFAULT_CONFIG() {0}
typedef struct { int dummy; } *httpd_handle_t;
typedef struct httpd_req {
	const char *uri; size_t content_len; char *test_body;
	char resp[4096]; size_t resp_len; char status[40]; int sent;
} httpd_req_t;
typedef bool (*httpd_uri_match_func_t)(const char*, const char*, size_t);
typedef struct { httpd_uri_match_func_t uri_match_fn; int max_uri_handlers, stack_size, max_open_sockets; bool lru_purge_enable; } httpd_config_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t*); } httpd_uri_t;
static inline bool httpd_uri_match_wildcard(const char*a,const char*b,size_t c){(void)a;(void)b;(void)c;return true;}
static inline esp_err_t httpd_start(httpd_handle_t*h,httpd_config_t*c){(void)h;(void)c;return 0;}
static inline esp_err_t httpd_stop(httpd_handle_t h){(void)h;return 0;}
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t h,const httpd_uri_t*u){(void)h;(void)u;return 0;}
static inline esp_err_t httpd_resp_set_type(httpd_req_t*r,const char*t){(void)r;(void)t;return 0;}
static inline esp_err_t httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){(void)r;(void)k;(void)v;return 0;}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, long len);
#define HTTPD_SOCK_ERR_TIMEOUT (-408)
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
