#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
esp_err_t ncp_flash_begin(void);
esp_err_t ncp_flash_feed(const uint8_t *data, size_t len);
esp_err_t ncp_flash_end(void);
void      ncp_flash_abort(void);
size_t    ncp_flash_sent(void);
bool      ncp_flash_active(void);
