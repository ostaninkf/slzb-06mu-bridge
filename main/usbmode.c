/* Проброс радиомодуля в USB — второй режим работы моста, как в штатной
 * прошивке («Coordinator mode: USB»). Радио отдаётся не в сеть, а в USB-CDC:
 * так мост можно воткнуть прямо в машину с Z2M или прошивальщиком.
 *
 * На ESP32-S3 USB-CDC и консоль живут на одном контроллере, поэтому в этом
 * режиме консоль замолкает — журнал остаётся только в syslog, если он включён. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "board.h"
#include "settings.h"
#include "usbmode.h"

static const char *TAG = "usb";

static void usb_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    uint8_t buf[512];
    for (;;) {
        esp_task_wdt_reset();

        int n = uart_read_bytes(NCP_UART_NUM, buf, sizeof(buf), 0);
        if (n > 0) usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(100));

        n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(5));
        if (n > 0) uart_write_bytes(NCP_UART_NUM, buf, n);
    }
}

void usbmode_start(void)
{
    if (cfg.ncp_route != NCP_TO_USB) return;

    usb_serial_jtag_driver_config_t c = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 4096,
    };
    if (usb_serial_jtag_driver_install(&c) != ESP_OK) {
        ESP_LOGE(TAG, "USB-порт не открылся, остаюсь в сетевом режиме");
        return;
    }
    esp_log_level_set("*", ESP_LOG_NONE);   /* консоль занята данными радио */
    xTaskCreatePinnedToCore(usb_task, "usbmode", 4096, NULL, 11, NULL, 1);
}
