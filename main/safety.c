/* Автооткат на предыдущую прошивку.
 *
 * Работает в два слоя:
 *
 * 1. Испытательный срок. После обновления образ считается непроверенным, пока
 *    мост не докажет, что жив: поднялся канал, получен адрес, радио отвечает.
 *    Пять минут без этого — и мост сам возвращает загрузочный раздел на
 *    предыдущий и перезагружается.
 *
 * 2. Счётчик аварийных перезагрузок. Если образ падает в панику или его снимает
 *    сторожевой таймер, счётчик растёт; на третий раз мост уходит на прошлый
 *    образ, не дожидаясь испытательного срока.
 *
 * Второй слой нужен потому, что штатный откат ESP-IDF выполняет загрузчик, а
 * его обновляют только по USB — здесь же откат делает само приложение, и этого
 * хватает во всех случаях, кроме падения до старта FreeRTOS. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "bridge.h"
#include "net.h"
#include "safety.h"

static const char *TAG = "safety";
static const char *NS = "safety";

#define TRIAL_SECONDS   300
#define MAX_CRASHES     3

static bool   on_trial;
static bool   confirmed;
static int    trial_left;
static uint8_t crashes;

bool safety_on_trial(void) { return on_trial && !confirmed; }
int  safety_trial_left(void) { return safety_on_trial() ? trial_left : 0; }
uint8_t safety_crashes(void) { return crashes; }

static void nvs_set_u8_commit(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void remember_good(const char *label)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "good", label);
    nvs_set_u8(h, "crashes", 0);
    nvs_commit(h);
    nvs_close(h);
}

/* Переключиться на другой образ и перезагрузиться. */
static void rollback(const char *why)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (!other) { ESP_LOGE(TAG, "откатываться некуда"); return; }

    esp_app_desc_t d;
    if (esp_ota_get_partition_description(other, &d) != ESP_OK) {
        ESP_LOGE(TAG, "в запасном разделе нет пригодного образа — остаюсь здесь");
        nvs_set_u8_commit("crashes", 0);
        return;
    }
    ESP_LOGE(TAG, "откат на %s (%s): %s", other->label, d.version, why);
    nvs_set_u8_commit("crashes", 0);
    if (esp_ota_set_boot_partition(other) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

void safety_confirm(const char *why)
{
    if (confirmed || !on_trial) return;
    confirmed = true;
    const esp_partition_t *run = esp_ota_get_running_partition();
    remember_good(run->label);
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "образ признан рабочим (%s), испытательный срок снят", why);
}

static void safety_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    trial_left = TRIAL_SECONDS;

    while (trial_left > 0) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
        trial_left--;

        bridge_stats_t s;
        bridge_get_stats(&s);
        bool link = net_eth_link() || net_wifi_link();
        bool addr = net_ip()[0] >= '0' && net_ip()[0] <= '9';
        bool radio = s.ncp_to_net > 0 || s.dropped_no_client > 0;

        if (link && addr && radio) { safety_confirm("канал, адрес и радио в порядке"); break; }
    }

    if (!confirmed) rollback("за испытательный срок мост не ожил");

    /* Дальше задача живёт как сторож аварийных перезагрузок: счётчик обнуляется
     * только после часа непрерывной работы, иначе серия падений с интервалом
     * в минуты не наберётся и откат не сработает. */
    for (int i = 0; i < 3600; i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(1000)); }
    nvs_set_u8_commit("crashes", 0);
    ESP_LOGI(TAG, "час без падений — счётчик аварий сброшен");
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

void safety_start(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();

    char good[16] = "";
    size_t len = sizeof(good);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "good", good, &len);
        nvs_get_u8(h, "crashes", &crashes);
        nvs_close(h);
    }

    esp_reset_reason_t r = esp_reset_reason();
    bool crashed = (r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT ||
                    r == ESP_RST_INT_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
    if (crashed) {
        crashes++;
        nvs_set_u8_commit("crashes", crashes);
        ESP_LOGE(TAG, "предыдущий запуск оборвался (причина %d), аварий подряд: %u", r, crashes);
        if (crashes >= MAX_CRASHES) { rollback("три аварийные перезагрузки подряд"); return; }
    }

    on_trial = strcmp(good, run->label) != 0;
    ESP_LOGW(TAG, "работаю из %s, %s", run->label,
             on_trial ? "образ непроверенный — идёт испытательный срок"
                      : "образ уже признан рабочим");

    if (!on_trial) { esp_ota_mark_app_valid_cancel_rollback(); return; }
    xTaskCreate(safety_task, "safety", 4096, NULL, 5, NULL);
}
