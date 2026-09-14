/* Отправка журнала на удалённый syslog по UDP.
 *
 * Нужна ровно по той причине, по которой мы неделю ловили разрывы: когда мост
 * перезагружается, его собственный лог исчезает. На чужой машине он остаётся. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "settings.h"
#include "syslogc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SYSLOG_LINE 240
#define QUEUE_LEN  32

static int sock = -1;
static struct sockaddr_in dst;
static vprintf_like_t prev_logger;
static QueueHandle_t q;
static volatile bool inside;      /* защита от рекурсии */

/* Логгер только складывает строку в очередь. Отправляет отдельная задача:
 * вызывать сетевые функции прямо отсюда нельзя — lwIP пишет собственные
 * сообщения, логгер вызывается повторно, и стек кончается за миллисекунды. */
static int logger(const char *fmt, va_list ap)
{
    char line[SYSLOG_LINE];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (q && n > 0 && !inside && !xPortInIsrContext()) {
        /* Размер блока и объём копирования должны совпадать. Раньше здесь
         * выделялось n+1 байт, а писалось до SYSLOG_LINE — запись уходила за
         * границу блока и рушила кучу: мост падал с assert в аллокаторе. */
        size_t sz = (size_t)n + 1;
        if (sz > SYSLOG_LINE) sz = SYSLOG_LINE;
        char *copy = malloc(sz);
        if (copy) {
            memcpy(copy, line, sz - 1);
            copy[sz - 1] = 0;
            if (xQueueSend(q, &copy, 0) != pdTRUE) free(copy);
        }
    }
    return prev_logger ? prev_logger(fmt, ap) : n;
}

static void sender_task(void *arg)
{
    char *line;
    for (;;) {
        if (xQueueReceive(q, &line, portMAX_DELAY) != pdTRUE) continue;
        if (sock >= 0) {
            char msg[SYSLOG_LINE + 96];
            /* facility local0 (16), severity informational (6) => 134 */
            int m = snprintf(msg, sizeof(msg), "<134>%s %s", cfg.hostname, line);
            inside = true;
            sendto(sock, msg, m > 0 ? m : 0, 0, (struct sockaddr *)&dst, sizeof(dst));
            inside = false;
        }
        free(line);
    }
}

void syslog_start(void)
{
    if (!cfg.syslog_enabled || !cfg.syslog_host[0]) return;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE("syslog", "сокет не создан"); return; }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg.syslog_port);
    dst.sin_addr.s_addr = inet_addr(cfg.syslog_host);

    q = xQueueCreate(QUEUE_LEN, sizeof(char *));
    if (!q) { close(sock); sock = -1; return; }
    xTaskCreate(sender_task, "syslog", 4096, NULL, 2, NULL);
    prev_logger = esp_log_set_vprintf(logger);
    ESP_LOGI("syslog", "журнал уходит на %s:%u", cfg.syslog_host, cfg.syslog_port);
}
