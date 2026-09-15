#pragma once
/* Перехват журнала: ставится первой строкой app_main(), до того как известен
 * адрес назначения. Всё, что напечатано раньше syslog_start(), ждёт в очереди
 * и уходит, как только поднимется сеть. */
void syslog_capture_start(void);
void syslog_start(void);
