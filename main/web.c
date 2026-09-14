/* Небольшой веб-интерфейс: живой статус, обновление по сети, сброс NCP.
 * Без него после первой заливки по USB пришлось бы каждый раз лезть к мосту
 * физически — мост стоит на складе. */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bridge.h"
#include "board.h"

static const char *TAG = "web";

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><title>SLZB-06MU bridge</title>"
"<style>body{font:14px system-ui;margin:2rem;max-width:40rem}"
"table{border-collapse:collapse;width:100%}td{padding:.3rem .6rem;border-bottom:1px solid #ddd}"
"td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
"button{font:inherit;padding:.4rem .8rem;margin-right:.5rem}</style>"
"<h1>SLZB-06MU &mdash; мост NCP</h1><table id=t></table>"
"<p><button onclick=\"fetch('/ncp/reset',{method:'POST'})\">Сбросить NCP</button>"
"<button onclick=\"fetch('/reboot',{method:'POST'})\">Перезагрузить мост</button></p>"
"<p>Обновление: <code>curl -X POST --data-binary @slzb-bridge.bin http://ХОСТ/update</code></p>"
"<script>const N={uptime:'Аптайм, с',ncp_to_net:'NCP &rarr; сеть, байт',"
"net_to_ncp:'сеть &rarr; NCP, байт',ring_used:'в кольце сейчас',ring_full:'переполнений кольца',"
"connects:'подключений',disconnects:'обрывов',dropped_no_client:'отброшено без клиента',"
"ncp_resets:'сбросов NCP',heap:'свободная куча',min_heap:'минимум кучи',version:'версия'};"
"async function u(){const r=await(await fetch('/status')).json();"
"document.getElementById('t').innerHTML=Object.entries(r).map(([k,v])=>"
"`<tr><td>${N[k]||k}</td><td>${v}</td></tr>`).join('')}u();setInterval(u,2000);</script>";

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *r)
{
    bridge_stats_t s;
    bridge_get_stats(&s);
    const esp_app_desc_t *d = esp_app_get_description();
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime\":%llu,\"ncp_to_net\":%llu,\"net_to_ncp\":%llu,\"ring_used\":%u,"
        "\"ring_full\":%u,\"connects\":%u,\"disconnects\":%u,\"dropped_no_client\":%llu,"
        "\"ncp_resets\":%u,\"heap\":%u,\"min_heap\":%u,\"version\":\"%s %s\"}",
        (unsigned long long)(esp_timer_get_time() / 1000000),
        (unsigned long long)s.ncp_to_net, (unsigned long long)s.net_to_ncp,
        (unsigned)s.ring_used, (unsigned)s.ring_full,
        (unsigned)s.connects, (unsigned)s.disconnects,
        (unsigned long long)s.dropped_no_client, (unsigned)s.ncp_resets,
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        d->version, d->date);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n);
}

static esp_err_t h_ncp_reset(httpd_req_t *r)
{
    ncp_reset();
    return httpd_resp_send(r, "ok", 2);
}

static void reboot_task(void *a) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }

static esp_err_t h_reboot(httpd_req_t *r)
{
    httpd_resp_send(r, "ok", 2);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Обновление: тело запроса — сырой образ приложения, как его собирает idf.py.
 * Пишем во второй OTA-слот; если образ битый, esp_ota_end это заметит и
 * загрузочный слот не переключится. */
static esp_err_t h_update(httpd_req_t *r)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "нет OTA-раздела");

    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin");

    ESP_LOGW(TAG, "приём обновления (%d байт) в раздел %s", r->content_len, part->label);
    char buf[2048];
    int left = r->content_len;
    while (left > 0) {
        int n = httpd_req_recv(r, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n <= 0) { esp_ota_abort(h); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "обрыв приёма"); }
        if (esp_ota_write(h, buf, n) != ESP_OK) { esp_ota_abort(h); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write"); }
        left -= n;
    }
    if (esp_ota_end(h) != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "образ не прошёл проверку");
    if (esp_ota_set_boot_partition(part) != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot_partition");

    ESP_LOGW(TAG, "обновление принято, перезагрузка");
    httpd_resp_send(r, "ok, перезагружаюсь", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "веб-сервер не поднялся"); return; }

    const httpd_uri_t u[] = {
        { .uri = "/",          .method = HTTP_GET,  .handler = h_root },
        { .uri = "/status",    .method = HTTP_GET,  .handler = h_status },
        { .uri = "/ncp/reset", .method = HTTP_POST, .handler = h_ncp_reset },
        { .uri = "/reboot",    .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/update",    .method = HTTP_POST, .handler = h_update },
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(srv, &u[i]);
    ESP_LOGI(TAG, "веб-интерфейс на :80");
}
