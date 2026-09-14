/* Индикация. LED1 — состояние сети, LED2 — есть ли клиент на радио.
 * Обе линии инверсные: единица гасит.
 *
 * Ночной режим и полное отключение — из штатной прошивки: мост часто стоит
 * в жилой комнате, и пара ярких светодиодов ночью мешает. */
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "board.h"
#include "settings.h"
#include "net.h"
#include "bridge.h"
#include "leds.h"

static bool night_now(void)
{
    if (!cfg.night_mode) return false;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 120) return false;          /* время ещё не синхронизировано */
    int h = tm.tm_hour;
    return cfg.night_from <= cfg.night_to ? (h >= cfg.night_from && h < cfg.night_to)
                                          : (h >= cfg.night_from || h < cfg.night_to);
}

static void set(int pin, bool on) { gpio_set_level(pin, on ? 0 : 1); }

static void leds_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    bool blink = false;
    for (;;) {
        esp_task_wdt_reset();
        blink = !blink;

        bool dark = !cfg.leds_enabled || night_now();
        bridge_stats_t s;
        bridge_get_stats(&s);
        bool client = s.connects > s.disconnects;

        if (dark) { set(PIN_LED1, false); set(PIN_LED2, false); set(PIN_RJ45_LEDS, false); }
        else {
            /* сеть есть — ровно горит, нет — мигает */
            set(PIN_LED1, net_eth_link() || net_wifi_link() ? true : blink);
            /* клиент есть — ровно горит, идёт обмен — подмигивает */
            set(PIN_LED2, client);
            set(PIN_RJ45_LEDS, true);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void leds_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LED1) | (1ULL << PIN_LED2) | (1ULL << PIN_RJ45_LEDS),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    set(PIN_LED1, false); set(PIN_LED2, false); set(PIN_RJ45_LEDS, true);
    xTaskCreate(leds_task, "leds", 3072, NULL, 3, NULL);
}
