/* Прошивка радиомодуля EFR32MG21 через мост.
 *
 * У модуля есть Gecko Bootloader: он поднимается, если при сбросе удерживать
 * линию boot, и принимает .gbl по XMODEM-CRC блоками в 128 байт. Штатная
 * прошивка делала то же самое, только тянула файл с сервера производителя —
 * здесь файл приходит из браузера, наружу мост не ходит.
 *
 * На время прошивки мост приостанавливается: UART нужен целиком. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "board.h"
#include "bridge.h"
#include "ncpflash.h"

static const char *TAG = "ncpfw";

#define SOH 0x01
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18

static uint8_t  block[128];
static size_t   fill;
static uint8_t  seq;
static bool     active;
static size_t   sent_bytes;

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0;
    while (n--) {
        c ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
    }
    return c;
}

static int wait_byte(uint8_t want, int ms)
{
    uint8_t b;
    TickType_t t0 = xTaskGetTickCount();
    while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(ms)) {
        if (uart_read_bytes(NCP_UART_NUM, &b, 1, pdMS_TO_TICKS(100)) == 1) {
            if (b == want) return 0;
            if (b == CAN)  return -2;
        }
    }
    return -1;
}

static int send_block(void)
{
    if (fill == 0) return 0;
    memset(block + fill, 0xFF, sizeof(block) - fill);   /* хвост добивается 0xFF */

    uint8_t hdr[3] = { SOH, seq, (uint8_t)~seq };
    uint16_t c = crc16(block, sizeof(block));
    uint8_t tail[2] = { (uint8_t)(c >> 8), (uint8_t)c };

    for (int try = 0; try < 5; try++) {
        uart_write_bytes(NCP_UART_NUM, hdr, sizeof(hdr));
        uart_write_bytes(NCP_UART_NUM, block, sizeof(block));
        uart_write_bytes(NCP_UART_NUM, tail, sizeof(tail));
        uart_wait_tx_done(NCP_UART_NUM, pdMS_TO_TICKS(2000));
        if (wait_byte(ACK, 3000) == 0) {
            seq++; sent_bytes += fill; fill = 0;
            return 0;
        }
        ESP_LOGW(TAG, "блок %u не принят, повтор %d", seq, try + 1);
    }
    return -1;
}

esp_err_t ncp_flash_begin(void)
{
    if (active) return ESP_ERR_INVALID_STATE;
    bridge_pause(true);
    uart_flush_input(NCP_UART_NUM);

    /* сброс с удержанием boot — обе линии инверсные */
    gpio_set_level(PIN_NCP_BOOT, 0);
    gpio_set_level(PIN_NCP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NCP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PIN_NCP_BOOT, 1);

    /* меню загрузчика; пункт 1 — приём образа */
    uint8_t buf[256];
    int n = uart_read_bytes(NCP_UART_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(2000));
    if (n > 0) { buf[n] = 0; ESP_LOGI(TAG, "загрузчик: %.*s", n, buf); }

    uart_write_bytes(NCP_UART_NUM, "1", 1);
    if (wait_byte('C', 5000) != 0) {
        ESP_LOGE(TAG, "загрузчик не предложил приём образа");
        bridge_pause(false);
        return ESP_ERR_TIMEOUT;
    }

    seq = 1; fill = 0; sent_bytes = 0; active = true;
    ESP_LOGW(TAG, "приём образа радиомодуля начат");
    return ESP_OK;
}

esp_err_t ncp_flash_feed(const uint8_t *data, size_t len)
{
    if (!active) return ESP_ERR_INVALID_STATE;
    while (len) {
        size_t take = sizeof(block) - fill;
        if (take > len) take = len;
        memcpy(block + fill, data, take);
        fill += take; data += take; len -= take;
        if (fill == sizeof(block) && send_block() != 0) {
            ncp_flash_abort();
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t ncp_flash_end(void)
{
    if (!active) return ESP_ERR_INVALID_STATE;
    if (fill && send_block() != 0) { ncp_flash_abort(); return ESP_FAIL; }

    uint8_t eot = EOT;
    uart_write_bytes(NCP_UART_NUM, &eot, 1);
    int rc = wait_byte(ACK, 10000);

    uart_write_bytes(NCP_UART_NUM, "2", 1);      /* выйти из загрузчика и стартовать */
    vTaskDelay(pdMS_TO_TICKS(500));

    active = false;
    bridge_pause(false);
    ESP_LOGW(TAG, "образ передан: %u байт, итог %s", (unsigned)sent_bytes, rc == 0 ? "принят" : "не подтверждён");
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void ncp_flash_abort(void)
{
    if (!active) return;
    uint8_t can[2] = { CAN, CAN };
    uart_write_bytes(NCP_UART_NUM, can, sizeof(can));
    active = false;
    ncp_reset();
    bridge_pause(false);
    ESP_LOGE(TAG, "прошивка радиомодуля прервана");
}

size_t ncp_flash_sent(void) { return sent_bytes; }
bool   ncp_flash_active(void) { return active; }
