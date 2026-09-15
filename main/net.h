#pragma once
#include <stdbool.h>
#include <stdint.h>
void net_start(void);
void net_mac(uint8_t out[6]);
bool net_eth_link(void);
bool net_has_addr(void);
bool net_wifi_link(void);
bool net_ap_up(void);
const char *net_ip(void);
