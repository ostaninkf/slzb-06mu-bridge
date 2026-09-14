/* Мост NCP <-> TCP.
 *
 * Главные принципы (они же — выводы из разбора штатной SLZB-OS):
 *  - UART вычитывается в каждом обороте цикла, безусловно. Нет ни одной ветки,
 *    где чтение приостанавливается «до наступления события»: именно это в
 *    штатной прошивке приводило к залпам и таймаутам NCP.
 *  - Включён аппаратный RTS. Если разобрать кольцо не успеваем — NCP
 *    останавливают на физическом уровне, а не теряют данные молча.
 *  - Ни одного блокирующего вызова без таймаута; задача кормит task watchdog
 *    в каждом обороте.
 *  - Новое TCP-соединение всегда вытесняет старое: подвисший клиент не может
 *    заблокировать радио.
 */
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "board.h"
#include "bridge.h"

static const char *TAG = "bridge";

#define RING_SZ        16384   /* NCP -> сеть */
#define UART_RX_BUF    16384
#define UART_TX_BUF     8192
#define CHUNK           2048

/* кольцевой буфер NCP -> сеть */
static uint8_t ring[RING_SZ];
static size_t  r_head, r_tail;           /* head — куда пишем, tail — откуда шлём */

static int listen_fd = -1;
static int client_fd = -1;

static bridge_stats_t st;

static inline size_t ring_used(void) { return (r_head - r_tail) % RING_SZ; }
static inline size_t ring_free(void) { return RING_SZ - 1 - ring_used(); }

void bridge_get_stats(bridge_stats_t *out) { *out = st; out->ring_used = ring_used(); }

static void ring_put(const uint8_t *p, size_t n)
{
    while (n--) { ring[r_head] = *p++; r_head = (r_head + 1) % RING_SZ; }
}

static void close_client(const char *why)
{
    if (client_fd < 0) return;
    ESP_LOGW(TAG, "клиент отключён: %s (принято %llu, отдано %llu)", why,
             (unsigned long long)st.net_to_ncp, (unsigned long long)st.ncp_to_net);
    close(client_fd);
    client_fd = -1;
    st.disconnects++;
    r_tail = r_head;               /* остатки прошлой сессии в сеть не тащим */
}

static void ncp_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = NCP_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        /* Только RTS: мы тормозим NCP, но сами передаём всегда.
         * CTS намеренно не включён — если NCP не управляет этой линией,
         * включённый CTS намертво заблокировал бы передачу. Включать
         * UART_HW_FLOWCTRL_CTS_RTS только после проверки на живом NCP. */
        .flow_ctrl  = UART_HW_FLOWCTRL_RTS,
        .rx_flow_ctrl_thresh = 100,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(NCP_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(NCP_UART_NUM, PIN_NCP_TX, PIN_NCP_RX,
                                 PIN_NCP_RTS, PIN_NCP_CTS));
    ESP_ERROR_CHECK(uart_driver_install(NCP_UART_NUM, UART_RX_BUF, UART_TX_BUF,
                                        0, NULL, 0));
    ESP_LOGI(TAG, "UART2 %d бод, tx=%d rx=%d rts=%d (аппаратный RTS включён)",
             NCP_BAUD, PIN_NCP_TX, PIN_NCP_RX, PIN_NCP_RTS);
}

void ncp_reset(void)
{
    gpio_set_level(PIN_NCP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_NCP_RST, 1);
    st.ncp_resets++;
    ESP_LOGW(TAG, "NCP сброшен по линии RST");
}

static int listen_open(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(NCP_TCP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 2) < 0) {
        close(fd); return -1;
    }
    ESP_LOGI(TAG, "слушаю TCP :%d", NCP_TCP_PORT);
    return fd;
}

static void accept_new(void)
{
    struct sockaddr_in peer; socklen_t sl = sizeof(peer);
    int fd = accept(listen_fd, (struct sockaddr *)&peer, &sl);
    if (fd < 0) return;

    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    int idle = 10, intvl = 3, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));

    if (client_fd >= 0) close_client("вытеснен новым соединением");

    client_fd = fd;
    st.connects++;
    r_tail = r_head;               /* новая сессия начинается с чистого потока */
    uart_flush_input(NCP_UART_NUM);
    char ip[16];
    inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "клиент %s подключился", ip);
}

static void pump_uart_to_ring(void)
{
    uint8_t buf[CHUNK];
    for (;;) {
        size_t avail = ring_free();
        if (avail == 0) { st.ring_full++; return; }     /* RTS сам придержит NCP */
        size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
        int n = uart_read_bytes(NCP_UART_NUM, buf, want, 0);   /* без ожидания */
        if (n <= 0) return;
        if (client_fd < 0) { st.dropped_no_client += n; continue; }  /* дренируем */
        ring_put(buf, n);
        st.ncp_to_net += n;
    }
}

static bool flush_ring_to_net(void)
{
    while (ring_used() && client_fd >= 0) {
        size_t linear = (r_head >= r_tail) ? (r_head - r_tail) : (RING_SZ - r_tail);
        int n = send(client_fd, ring + r_tail, linear, MSG_DONTWAIT);
        if (n > 0) { r_tail = (r_tail + n) % RING_SZ; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false; /* ждём POLLOUT */
        close_client("ошибка записи в сокет");
        return true;
    }
    return true;
}

static void pump_net_to_uart(void)
{
    uint8_t buf[CHUNK];
    int n = recv(client_fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0) { close_client("закрыт с той стороны"); return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_client("ошибка чтения сокета");
        return;
    }
    /* uart_write_bytes копирует в буфер драйвера; при переполнении ждёт,
     * поэтому буфер TX взят с запасом, а порции ограничены CHUNK. */
    uart_write_bytes(NCP_UART_NUM, buf, n);
    st.net_to_ncp += n;
}

static void bridge_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while ((listen_fd = listen_open()) < 0) {
        esp_task_wdt_reset();
        ESP_LOGE(TAG, "не удалось открыть слушающий сокет, повтор через 2 с");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    for (;;) {
        esp_task_wdt_reset();

        struct pollfd p[2];
        int np = 0;
        p[np].fd = listen_fd; p[np].events = POLLIN; np++;
        if (client_fd >= 0) {
            p[np].fd = client_fd;
            p[np].events = POLLIN | (ring_used() ? POLLOUT : 0);
            np++;
        }

        int rc = poll(p, np, 5);          /* короткий таймаут: цикл не залипает */

        /* UART вычитывается всегда, независимо от исхода poll */
        pump_uart_to_ring();

        if (rc > 0) {
            if (p[0].revents & POLLIN) accept_new();
            if (np == 2 && client_fd >= 0) {
                if (p[1].revents & (POLLERR | POLLHUP)) close_client("POLLERR/POLLHUP");
                else {
                    if (p[1].revents & POLLIN)  pump_net_to_uart();
                    if (client_fd >= 0)         flush_ring_to_net();
                }
            }
        } else if (client_fd >= 0) {
            flush_ring_to_net();
        }
    }
}

void bridge_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_NCP_RST) | (1ULL << PIN_NCP_BOOT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_NCP_BOOT, 1);   /* обычный режим, не загрузчик */
    gpio_set_level(PIN_NCP_RST, 1);    /* сброс отпущен */

    ncp_uart_init();
    xTaskCreatePinnedToCore(bridge_task, "bridge", 5120, NULL, 10, NULL, 1);
}
