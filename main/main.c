/* SLZB-06MU: своя прошивка моста Zigbee-NCP <-> Ethernet.
 * ESP-IDF, без Arduino. Разбор штатной прошивки — ~/Claude/slzb/LOG.md. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "board.h"
#include "bridge.h"

void web_start(void);

static const char *TAG = "slzb";

static bool link_up = false;

/* У W5500 своего MAC нет, его задаёт хост. Штатная прошивка (Arduino) выводила
 * адрес из базового eFuse через esp_derive_local_mac() — тот же вывод повторён
 * здесь, поэтому после замены прошивки мост остаётся под прежним адресом и
 * резервация DHCP продолжает выдавать тот же IP.
 *
 * Если нужен другой адрес, положи рядом local_config.h (он не в репозитории):
 *     #define ETH_MAC_OVERRIDE { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 } */
#if defined(__has_include)
#  if __has_include("local_config.h")
#    include "local_config.h"
#  endif
#endif

static void eth_mac_get(uint8_t out[6])
{
#ifdef ETH_MAC_OVERRIDE
    const uint8_t fixed[6] = ETH_MAC_OVERRIDE;
    memcpy(out, fixed, 6);
#else
    /* Именно ESP_MAC_BASE, а не ESP_MAC_ETH: Ethernet-адрес в eFuse-наборе
     * смещён на +3 в последнем байте, и вывод из него даёт другой MAC —
     * мост получает от DHCP другой IP. Штатная прошивка на Arduino выводила
     * адрес из базового. */
    uint8_t base[6];
    ESP_ERROR_CHECK(esp_read_mac(base, ESP_MAC_BASE));
    ESP_ERROR_CHECK(esp_derive_local_mac(out, base));
#endif
}

/* --- индикация: LED1 — питание/жизнь, LED2 — есть клиент --- */
static void leds_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LED1) | (1ULL << PIN_LED2) | (1ULL << PIN_RJ45_LEDS),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_LED1, 1);        /* инверсные: 1 = погашен */
    gpio_set_level(PIN_LED2, 1);
    gpio_set_level(PIN_RJ45_LEDS, 0);   /* инверсный: 0 = светодиоды RJ45 включены */
}

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6] = {0};
        esp_eth_ioctl(*(esp_eth_handle_t *)data, ETH_CMD_G_MAC_ADDR, mac);
        link_up = true;
        ESP_LOGI(TAG, "link up, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        link_up = false;
        ESP_LOGW(TAG, "link down");
        break;
    case ETHERNET_EVENT_START: ESP_LOGI(TAG, "ethernet запущен"); break;
    case ETHERNET_EVENT_STOP:  ESP_LOGW(TAG, "ethernet остановлен"); break;
    }
}

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "адрес получен: " IPSTR " маска " IPSTR " шлюз " IPSTR,
             IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));
    gpio_set_level(PIN_LED1, 0);        /* есть сеть — LED1 горит */
}

/* Инициализация Ethernet. Возвращает ошибку вместо паники: если W5500 не
 * ответил (сбой SPI, просадка питания), мост должен повторить попытку, а не
 * уйти в бесконечную перезагрузку. */
static esp_err_t eth_try_init(void)
{
    esp_err_t err;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_eth_handle_t eth = NULL;

    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = W5500_SPI_HZ,
        .spics_io_num = PIN_W5500_CS,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &dev);
    w5500.int_gpio_num = PIN_W5500_INT;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = PIN_W5500_RST;

    mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!mac || !phy) { err = ESP_FAIL; goto fail; }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_cfg, &eth);
    if (err != ESP_OK) goto fail;

    uint8_t mac_addr[6];
    eth_mac_get(mac_addr);
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    err = esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, mac_addr);
    if (err != ESP_OK) goto fail_installed;

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(netif, "slzb-06mu");
    err = esp_netif_attach(netif, esp_eth_new_netif_glue(eth));
    if (err != ESP_OK) goto fail_installed;

    err = esp_eth_start(eth);
    if (err != ESP_OK) goto fail_installed;
    return ESP_OK;

fail_installed:
    esp_eth_driver_uninstall(eth);
    return err;
fail:
    if (mac) mac->del(mac);
    if (phy) phy->del(phy);
    ESP_LOGE(TAG, "инициализация W5500 не удалась: %s", esp_err_to_name(err));
    return err;
}

static void eth_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(W5500_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip, NULL));

    for (int attempt = 1; ; attempt++) {
        if (eth_try_init() == ESP_OK) return;
        ESP_LOGE(TAG, "попытка %d поднять Ethernet неудачна, повтор через 3 с", attempt);
        if (attempt >= 10) {
            ESP_LOGE(TAG, "W5500 не отвечает десять раз подряд — перезагружаюсь");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* Периодический отчёт: то, чего не хватало при разборе штатной прошивки —
 * видеть, что происходило на мосту в момент разрыва. */
static void stats_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    bridge_stats_t prev = {0};
    for (;;) {
        for (int i = 0; i < 30; i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(1000)); }
        bridge_stats_t s;
        bridge_get_stats(&s);
        ESP_LOGI(TAG,
                 "link=%d клиент=%s | NCP->сеть %llu (+%llu) | сеть->NCP %llu (+%llu) | "
                 "кольцо %u/%d, переполнений %u | коннектов %u, обрывов %u | "
                 "потеряно без клиента %llu | куча %u",
                 link_up, s.connects > s.disconnects ? "есть" : "нет",
                 (unsigned long long)s.ncp_to_net, (unsigned long long)(s.ncp_to_net - prev.ncp_to_net),
                 (unsigned long long)s.net_to_ncp, (unsigned long long)(s.net_to_ncp - prev.net_to_ncp),
                 (unsigned)s.ring_used, 16384, (unsigned)s.ring_full,
                 (unsigned)s.connects, (unsigned)s.disconnects,
                 (unsigned long long)s.dropped_no_client,
                 (unsigned)esp_get_free_heap_size());
        gpio_set_level(PIN_LED2, s.connects > s.disconnects ? 0 : 1);
        prev = s;
    }
}

/* Кнопка на корпусе: короткое нажатие — сброс NCP, удержание пяти секунд —
 * перезагрузка моста. Единственное, что можно сделать руками, не разбирая. */
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
        while (gpio_get_level(PIN_BUTTON) == 0 && held < 6000) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(50));
            held += 50;
        }
        if (held >= 5000) { ESP_LOGW(TAG, "кнопка удержана — перезагрузка"); esp_restart(); }
        if (held >= 100)  ncp_reset();
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    leds_init();
    eth_init();
    bridge_start();
    web_start();
    xTaskCreate(stats_task, "stats", 4096, NULL, 3, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "мост запущен: NCP на UART2, порт %d", NCP_TCP_PORT);
}
