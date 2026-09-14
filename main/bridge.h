#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_netif.h"

typedef struct {
    uint64_t ncp_to_net;
    uint64_t net_to_ncp;
    uint64_t dropped_no_client;
    uint32_t connects;
    uint32_t disconnects;
    uint32_t ring_full;
    uint32_t ncp_resets;
    size_t   ring_used;
} bridge_stats_t;

void bridge_start(void);
void bridge_get_stats(bridge_stats_t *out);
void ncp_reset(void);
void bridge_pause(bool on);      /* приостановить мост: UART нужен целиком */
float bridge_chip_temp(void);
