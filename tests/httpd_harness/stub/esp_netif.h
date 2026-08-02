#pragma once
#include <stdint.h>
#define ESP_OK 0
typedef int esp_err_t;
typedef struct esp_netif_obj esp_netif_t;
typedef struct { struct { uint32_t addr; } ip, netmask, gw; } esp_netif_ip_info_t;
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k) { (void)k; return 0; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) { (void)n; (void)i; return -1; }
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(a) 0u,0u,0u,0u
