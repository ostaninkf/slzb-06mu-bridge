/* Отправка журнала на удалённый syslog по UDP.
 *
 * Нужна ровно по той причине, по которой мы неделю ловили разрывы: когда мост
 * перезагружается, его собственный лог исчезает. На чужой машине он остаётся. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "settings.h"
#include "syslogc.h"

static int sock = -1;
static struct sockaddr_in dst;
static vprintf_like_t prev_logger;

static int logger(const char *fmt, va_list ap)
{
    char line[512];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (sock >= 0 && n > 0) {
        char msg[600];
        /* facility local0 (16), severity informational (6) => 134 */
        int m = snprintf(msg, sizeof(msg), "<134>%s %s", cfg.hostname, line);
        sendto(sock, msg, m > 0 ? m : 0, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
    return prev_logger ? prev_logger(fmt, ap) : n;
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

    prev_logger = esp_log_set_vprintf(logger);
    ESP_LOGI("syslog", "журнал уходит на %s:%u", cfg.syslog_host, cfg.syslog_port);
}
