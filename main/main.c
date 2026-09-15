/* SLZB-06MU: своя прошивка моста Zigbee-NCP <-> Ethernet.
 * ESP-IDF, без Arduino. Разбор штатной прошивки — ~/Claude/slzb/LOG.md. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "board.h"
#include "settings.h"
#include "bridge.h"
#include "net.h"
#include "web.h"
#include "leds.h"
#include "mqttpub.h"
#include "syslogc.h"
#include "usbmode.h"
#include "safety.h"

static const char *TAG = "slzb";

/* Кнопка на корпусе: короткое нажатие — сброс радио, удержание пяти секунд —
 * перезагрузка моста, пятнадцати — сброс настроек к заводским.
 * Единственное, что можно сделать руками, не разбирая корпус. */
static void button_task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_BUTTON, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        esp_task_wdt_reset();
        if (gpio_get_level(PIN_BUTTON) != 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        int held = 0;
        while (gpio_get_level(PIN_BUTTON) == 0 && held < 16000) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(50));
            held += 50;
        }
        if (held >= 15000) {
            ESP_LOGW(TAG, "кнопка удержана 15 с — сбрасываю настройки");
            settings_defaults(&cfg);
            settings_save();
            esp_restart();
        }
        if (held >= 5000) { ESP_LOGW(TAG, "кнопка удержана — перезагрузка"); esp_restart(); }
        if (held >= 100)  ncp_reset();
    }
}

static void stats_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    bridge_stats_t prev = {0};
    for (;;) {
        for (int i = 0; i < 30; i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(1000)); }
        bridge_stats_t s;
        bridge_get_stats(&s);
        ESP_LOGI(TAG,
                 "канал=%s %s | клиент=%s | радио->сеть %llu (+%llu) | сеть->радио %llu (+%llu) | "
                 "кольцо %u, переполнений %u | коннектов %u, обрывов %u | %.1f °C | куча %u",
                 net_eth_link() ? "витая пара" : net_wifi_link() ? "Wi-Fi" : "нет", net_ip(),
                 s.client ? "есть" : "нет",
                 (unsigned long long)s.ncp_to_net, (unsigned long long)(s.ncp_to_net - prev.ncp_to_net),
                 (unsigned long long)s.net_to_ncp, (unsigned long long)(s.net_to_ncp - prev.net_to_ncp),
                 (unsigned)s.ring_used, (unsigned)s.ring_full,
                 (unsigned)s.connects, (unsigned)s.disconnects,
                 bridge_chip_temp(), (unsigned)esp_get_free_heap_size());
        prev = s;
    }
}

void app_main(void)
{
    /* Перехват журнала — раньше всего остального. Пока syslog_start() стоял
     * после net_start(), всё, что печаталось при старте, уходило только в
     * консоль, которой на PoE-мосту никто не смотрит: так полтора суток
     * пряталась ошибка драйвера GPIO про необработанное прерывание W5500.
     * Строки ждут в очереди и уходят, как только появится адрес. */
    syslog_capture_start();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Раньше всего: счётчик аварийных перезагрузок и решение об откате.
     * Если поставить это после инициализации периферии, падение внутри неё
     * не попадёт в счётчик — мост уйдёт в бесконечный цикл перезагрузок,
     * и вернуть его можно будет только по USB. Так и случилось 14.09.2026. */
    safety_start();

    settings_load();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    leds_start();
    net_start();
    syslog_start();
    bridge_start();
    usbmode_start();
    web_start();
    mqtt_start();

    xTaskCreate(stats_task, "stats", 4096, NULL, 3, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "мост запущен: радио на UART2, режим %s",
             cfg.ncp_route == NCP_TO_USB ? "USB" : "сеть");
}
