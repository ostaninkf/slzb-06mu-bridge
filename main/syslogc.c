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
#include "net.h"
#include "syslogc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* 240 байт резали строку статистики посередине: она набирается кириллицей по
 * два байта на знак, и до удалённого журнала не доезжали ни число обрывов, ни
 * температура, ни куча — полторы недели в /var/log/slzb-bridge.log каждая
 * периодическая запись обрывалась на «…коннектов 11, обрыв». */
#define SYSLOG_LINE 512
#define QUEUE_LEN    64           /* хватает, чтобы пережить весь ранний старт */

static int sock = -1;
static struct sockaddr_in dst;
static vprintf_like_t prev_logger;
static QueueHandle_t q;
static TaskHandle_t sender;

/* Обрезка по границе символа: иначе на конце строки остаётся половина буквы,
 * и получатель показывает битый байт. */
static size_t utf8_fit(const char *s, size_t n)
{
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;   /* назад к ведущему */
    if (i == 0) return n;
    unsigned char c = (unsigned char)s[i - 1];
    size_t need = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    return i - 1 + need <= n ? n : i - 1;     /* символ влез целиком — оставляем */
}

/* Логгер только складывает строку в очередь. Отправляет отдельная задача:
 * вызывать сетевые функции прямо отсюда нельзя — lwIP пишет собственные
 * сообщения, логгер вызывается повторно, и стек кончается за миллисекунды.
 *
 * Глушим при этом ровно одну задачу — свою же отправляющую, иначе получается
 * петля «send -> сообщение -> send». Прежний глобальный флаг на время sendto()
 * ронял и чужие сообщения: терялось то, что печатали другие задачи. */
static int logger(const char *fmt, va_list ap)
{
    char line[SYSLOG_LINE];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (q && n > 0 && xTaskGetCurrentTaskHandle() != sender && !xPortInIsrContext()) {
        /* Размер блока и объём копирования должны совпадать. Раньше здесь
         * выделялось n+1 байт, а писалось до SYSLOG_LINE — запись уходила за
         * границу блока и рушила кучу: мост падал с assert в аллокаторе. */
        size_t sz = (size_t)n + 1;
        if (sz > SYSLOG_LINE) sz = utf8_fit(line, SYSLOG_LINE - 1) + 1;
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
    /* Пока адреса нет, строки копятся в очереди: ради ранней диагностики
     * перехват и поднят в самое начало app_main(). */
    while (!net_has_addr()) vTaskDelay(pdMS_TO_TICKS(200));
    for (;;) {
        if (xQueueReceive(q, &line, portMAX_DELAY) != pdTRUE) continue;
        if (sock >= 0) {
            char msg[SYSLOG_LINE + 96];
            /* facility local0 (16), severity informational (6) => 134 */
            int m = snprintf(msg, sizeof(msg), "<134>%s %s", cfg.hostname, line);
            sendto(sock, msg, m > 0 ? m : 0, 0, (struct sockaddr *)&dst, sizeof(dst));
        }
        free(line);
    }
}

void syslog_capture_start(void)
{
    q = xQueueCreate(QUEUE_LEN, sizeof(char *));
    if (q) prev_logger = esp_log_set_vprintf(logger);
}

/* Отправлять некому: снимаем перехват и отдаём накопленное.
 *
 * Саму очередь не удаляем. К этому моменту задачи уже работают и пишут в
 * журнал: та, что успела прочитать q до обнуления, положит строку в очередь
 * уже после — в удалённую это было бы падением. Очередь ограничена сверху,
 * так что осядет в ней самое большее QUEUE_LEN строк. */
static void capture_stop(void)
{
    if (!q) return;
    QueueHandle_t held = q;
    q = NULL;
    if (prev_logger) esp_log_set_vprintf(prev_logger);
    char *line;
    while (xQueueReceive(held, &line, 0) == pdTRUE) free(line);
}

void syslog_start(void)
{
    if (!cfg.syslog_enabled || !cfg.syslog_host[0]) { capture_stop(); return; }
    if (!q) syslog_capture_start();
    if (!q) return;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE("syslog", "сокет не создан"); capture_stop(); return; }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg.syslog_port);
    dst.sin_addr.s_addr = inet_addr(cfg.syslog_host);

    xTaskCreate(sender_task, "syslog", 4096, NULL, 2, &sender);
    ESP_LOGI("syslog", "журнал уходит на %s:%u", cfg.syslog_host, cfg.syslog_port);
}
