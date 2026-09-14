/* Публикация состояния в MQTT — тем же набором, что собирал внешний скрипт,
 * но изнутри прошивки: discovery для Home Assistant плюс метрики раз в 15 с.
 *
 * Смысл — видеть причину, а не симптом: обрывы сокета, переполнения кольца и
 * сброшенные кадры публикуются наравне с аптаймом. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "settings.h"
#include "bridge.h"
#include "net.h"
#include "mqttpub.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t client;
static bool online;

static void pub(const char *sub, const char *val, int retain)
{
    if (!client || !online) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s", cfg.mqtt_base, sub);
    esp_mqtt_client_publish(client, topic, val, 0, 0, retain);
}

/* один сенсор HA-discovery */
static void disc(const char *uid, const char *name, const char *sub, const char *extra)
{
    char topic[160], payload[640];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_%s/config", cfg.mqtt_base, uid);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s/%s\","
        "\"availability_topic\":\"%s/available\",\"expire_after\":120,%s"
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"SLZB-06MU мост\","
        "\"manufacturer\":\"SMLIGHT\",\"model\":\"своя прошивка\"}}",
        name, cfg.mqtt_base, uid, cfg.mqtt_base, sub, cfg.mqtt_base, extra, cfg.mqtt_base);
    esp_mqtt_client_publish(client, topic, payload, 0, 0, 1);
}

static void announce(void)
{
    disc("uptime",      "Мост: аптайм",              "uptime",      "\"unit_of_measurement\":\"s\",\"state_class\":\"measurement\",");
    disc("clients",     "Мост: клиентов на радио",   "clients",     "");
    disc("disconnects", "Мост: обрывов сокета",      "disconnects", "\"state_class\":\"total_increasing\",");
    disc("ring_full",   "Мост: переполнений буфера", "ring_full",   "\"state_class\":\"total_increasing\",");
    disc("ncp_to_net",  "Мост: байт от радио",       "ncp_to_net",  "\"state_class\":\"total_increasing\",\"unit_of_measurement\":\"B\",");
    disc("net_to_ncp",  "Мост: байт к радио",        "net_to_ncp",  "\"state_class\":\"total_increasing\",\"unit_of_measurement\":\"B\",");
    disc("heap",        "Мост: свободная память",    "heap",        "\"state_class\":\"measurement\",\"unit_of_measurement\":\"B\",");
    disc("temp",        "Мост: температура чипа",    "temp",        "\"device_class\":\"temperature\",\"unit_of_measurement\":\"°C\",\"state_class\":\"measurement\",");
    pub("available", "online", 1);
}

static void ev(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        online = true;
        ESP_LOGI(TAG, "подключён к брокеру %s", cfg.mqtt_host);
        announce();
        break;
    case MQTT_EVENT_DISCONNECTED:
        online = false;
        ESP_LOGW(TAG, "брокер отвалился");
        break;
    default: break;
    }
}

static void mqtt_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    char v[32];
    for (;;) {
        for (int i = 0; i < 15; i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(1000)); }
        if (!online) continue;

        bridge_stats_t s;
        bridge_get_stats(&s);
        snprintf(v, sizeof(v), "%llu", (unsigned long long)(esp_timer_get_time() / 1000000)); pub("uptime", v, 0);
        snprintf(v, sizeof(v), "%d", s.connects > s.disconnects ? 1 : 0);                     pub("clients", v, 0);
        snprintf(v, sizeof(v), "%u", (unsigned)s.disconnects);                                pub("disconnects", v, 0);
        snprintf(v, sizeof(v), "%u", (unsigned)s.ring_full);                                  pub("ring_full", v, 0);
        snprintf(v, sizeof(v), "%llu", (unsigned long long)s.ncp_to_net);                     pub("ncp_to_net", v, 0);
        snprintf(v, sizeof(v), "%llu", (unsigned long long)s.net_to_ncp);                     pub("net_to_ncp", v, 0);
        snprintf(v, sizeof(v), "%u", (unsigned)esp_get_free_heap_size());                     pub("heap", v, 0);
        snprintf(v, sizeof(v), "%.1f", bridge_chip_temp());                                   pub("temp", v, 0);
    }
}

void mqtt_start(void)
{
    if (!cfg.mqtt_enabled || !cfg.mqtt_host[0]) { ESP_LOGI(TAG, "публикация выключена"); return; }

    char uri[128], lwt[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", cfg.mqtt_host, cfg.mqtt_port);
    snprintf(lwt, sizeof(lwt), "%s/available", cfg.mqtt_base);

    esp_mqtt_client_config_t c = {
        .broker.address.uri = uri,
        .credentials.username = cfg.mqtt_user[0] ? cfg.mqtt_user : NULL,
        .credentials.authentication.password = cfg.mqtt_pass[0] ? cfg.mqtt_pass : NULL,
        .credentials.client_id = cfg.hostname,
        .session.last_will.topic = lwt,
        .session.last_will.msg = "offline",
        .session.last_will.retain = 1,
        .session.keepalive = 30,
    };
    client = esp_mqtt_client_init(&c);
    if (!client) { ESP_LOGE(TAG, "клиент не создан"); return; }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, ev, NULL);
    esp_mqtt_client_start(client);
    xTaskCreate(mqtt_task, "mqttpub", 4096, NULL, 3, NULL);
}
