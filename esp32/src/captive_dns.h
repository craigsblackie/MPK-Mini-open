#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

#include <stdint.h>

typedef struct captive_dns *captive_dns_handle_t;

/* Answer every IPv4 DNS query with the SoftAP address. */
captive_dns_handle_t captive_dns_start(uint32_t ap_addr);

/* Stop the task, close its socket, and release its memory. */
void captive_dns_stop(captive_dns_handle_t handle);

#endif /* CAPTIVE_DNS_H */
