/* Сеть: Ethernet по W5500 (основной канал), Wi-Fi как резерв, точка доступа
 * для первичной настройки, mDNS и синхронизация времени.
 *
 * Wi-Fi поднимается только когда витой пары нет: у моста, стоящего на PoE,
 * радиоканал — страховка, а не рабочий режим. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "mdns.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "board.h"
#include "settings.h"
#include "net.h"

static const char *TAG = "net";

static esp_netif_t *eth_netif, *sta_netif, *ap_netif;
static bool eth_link, wifi_link, ap_up;
static char ip_str[32] = "нет адреса";

bool net_eth_link(void)   { return eth_link; }
bool net_has_addr(void)   { return ip_str[0] >= '0' && ip_str[0] <= '9'; }
bool net_wifi_link(void)  { return wifi_link; }
bool net_ap_up(void)      { return ap_up; }
const char *net_ip(void)  { return ip_str; }

void net_mac(uint8_t out[6])
{
    if (cfg.mac_override) { memcpy(out, cfg.mac, 6); return; }
    /* Именно ESP_MAC_BASE: Ethernet-адрес в eFuse-наборе смещён на +3 в
     * последнем байте, и вывод из него даёт другой MAC — мост получает от
     * DHCP другой адрес. Штатная прошивка выводила адрес из базового. */
    uint8_t base[6];
    ESP_ERROR_CHECK(esp_read_mac(base, ESP_MAC_BASE));
    ESP_ERROR_CHECK(esp_derive_local_mac(out, base));
}

static void apply_static_ip(esp_netif_t *nif)
{
    if (cfg.eth_dhcp || !cfg.eth_ip[0]) return;
    esp_netif_dhcpc_stop(nif);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = esp_ip4addr_aton(cfg.eth_ip);
    ip.netmask.addr = esp_ip4addr_aton(cfg.eth_mask[0] ? cfg.eth_mask : "255.255.255.0");
    ip.gw.addr      = esp_ip4addr_aton(cfg.eth_gw);
    esp_netif_set_ip_info(nif, &ip);

    if (cfg.dns1[0]) {
        esp_netif_dns_info_t d = {0};
        d.ip.type = ESP_IPADDR_TYPE_V4;
        d.ip.u_addr.ip4.addr = esp_ip4addr_aton(cfg.dns1);
        esp_netif_set_dns_info(nif, ESP_NETIF_DNS_MAIN, &d);
    }
    if (cfg.dns2[0]) {
        esp_netif_dns_info_t d = {0};
        d.ip.type = ESP_IPADDR_TYPE_V4;
        d.ip.u_addr.ip4.addr = esp_ip4addr_aton(cfg.dns2);
        esp_netif_set_dns_info(nif, ESP_NETIF_DNS_BACKUP, &d);
    }
    ESP_LOGI(TAG, "статический адрес %s/%s, шлюз %s", cfg.eth_ip, cfg.eth_mask, cfg.eth_gw);
}

/* --- Wi-Fi поднимаем/опускаем по состоянию витой пары --- */
static void wifi_start_sta(void)
{
    if (!cfg.wifi_enabled || !cfg.wifi_ssid[0]) return;
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, cfg.wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, cfg.wifi_pass, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    ESP_LOGW(TAG, "линка по витой паре нет — поднимаю Wi-Fi «%s»", cfg.wifi_ssid);
}

static void wifi_start_ap(void)
{
    if (!cfg.ap_fallback || ap_up) return;
    uint8_t mac[6]; net_mac(mac);
    wifi_config_t wc = {0};
    int n = snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "%s-%02X%02X",
                     cfg.hostname, mac[4], mac[5]);
    wc.ap.ssid_len = n;
    wc.ap.max_connection = 2;
    wc.ap.authmode = cfg.ap_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    strncpy((char *)wc.ap.password, cfg.ap_pass, sizeof(wc.ap.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ap_up = true;
    ESP_LOGW(TAG, "поднята точка доступа «%s» для настройки", wc.ap.ssid);
}

static void wifi_stop_all(void)
{
    if (!ap_up && !wifi_link) return;
    esp_wifi_stop();
    ap_up = wifi_link = false;
    ESP_LOGI(TAG, "Wi-Fi выключен: работает витая пара");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT) {
        if (id == ETHERNET_EVENT_CONNECTED) {
            eth_link = true;
            ESP_LOGI(TAG, "линк есть");
            wifi_stop_all();
        } else if (id == ETHERNET_EVENT_DISCONNECTED) {
            eth_link = false;
            snprintf(ip_str, sizeof(ip_str), "нет адреса");
            ESP_LOGW(TAG, "линк пропал");
            wifi_start_sta();
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_link = false;
            if (!eth_link) { vTaskDelay(pdMS_TO_TICKS(3000)); esp_wifi_connect(); }
        }
    } else if (base == IP_EVENT) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        if (id == IP_EVENT_STA_GOT_IP) wifi_link = true;
        ESP_LOGI(TAG, "адрес %s", ip_str);
    }
}

/* --- W5500 --- */
static esp_err_t eth_try_init(void)
{
    esp_err_t err = ESP_FAIL;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_eth_handle_t eth = NULL;
    esp_eth_netif_glue_handle_t glue = NULL;

    spi_device_interface_config_t dev = {
        .mode = 0, .clock_speed_hz = W5500_SPI_HZ,
        .spics_io_num = PIN_W5500_CS, .queue_size = 20,
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
    if (!mac || !phy) goto fail;

    /* Без установленного сервиса прерываний gpio_isr_handler_add() внутри
     * драйвера W5500 возвращает ESP_ERR_INVALID_STATE, а IDF этот возврат не
     * проверяет: прерывание INT не доходит никогда, и задача приёма опускается
     * на страховочный опрос ulTaskNotifyTake(..., pdMS_TO_TICKS(1000)).
     * Пакеты в этом режиме разбираются пачкой раз в секунду — измеренный ping
     * 74…1101 мс пилой. Ставить до esp_eth_driver_install: он вызывает
     * mac->init(), а тот уже цепляет обработчик. */
    esp_err_t isr = gpio_install_isr_service(0);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "сервис прерываний GPIO не встал: %s — приём пойдёт "
                      "опросом раз в секунду", esp_err_to_name(isr));
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_cfg, &eth);
    if (err != ESP_OK) goto fail;

    uint8_t mac_addr[6];
    net_mac(mac_addr);
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1],
             mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    err = esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, mac_addr);
    if (err != ESP_OK) goto fail;

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&netif_cfg);
    if (!eth_netif) { err = ESP_ERR_NO_MEM; goto fail; }
    esp_netif_set_hostname(eth_netif, cfg.hostname);
    glue = esp_eth_new_netif_glue(eth);
    if (!glue) { err = ESP_ERR_NO_MEM; goto fail; }
    err = esp_netif_attach(eth_netif, glue);
    if (err != ESP_OK) goto fail;
    glue = NULL;                    /* дальше связка принадлежит netif */
    apply_static_ip(eth_netif);

    err = esp_eth_start(eth);
    if (err != ESP_OK) goto fail;
    return ESP_OK;

/* Разбирать за собой обязательно: каждая попытка заводит устройство на шине
 * SPI, netif и связку между ними, а попыток здесь десять подряд. Прежний код
 * на пути «драйвер уже установлен» не освобождал ни mac, ни phy, ни netif —
 * до перезагрузки доживала горсть повисших комплектов. */
fail:
    if (glue)      esp_eth_del_netif_glue(glue);
    if (eth_netif) { esp_netif_destroy(eth_netif); eth_netif = NULL; }
    if (eth)       esp_eth_driver_uninstall(eth);
    if (mac)       mac->del(mac);
    if (phy)       phy->del(phy);
    ESP_LOGE(TAG, "инициализация W5500 не удалась: %s", esp_err_to_name(err));
    return err;
}

/* Если ни витой пары, ни Wi-Fi нет — через минуту поднимаем точку доступа,
 * чтобы мост можно было настроить, не разбирая корпус. */
static void fallback_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(60000));
    for (;;) {
        if (!eth_link && !wifi_link && !ap_up) wifi_start_ap();
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void net_start(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(W5500_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT,  ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    /* Wi-Fi инициализируем всегда, но не включаем: радио стартует только
     * как резерв, чтобы не тратить питание и эфир на PoE-устройстве. */
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif  = esp_netif_create_default_wifi_ap();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    for (int attempt = 1; ; attempt++) {
        if (eth_try_init() == ESP_OK) break;
        ESP_LOGE(TAG, "попытка %d поднять Ethernet неудачна, повтор через 3 с", attempt);
        if (attempt >= 10) {
            ESP_LOGE(TAG, "W5500 не отвечает десять раз подряд — перезагружаюсь");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(cfg.hostname);
    mdns_instance_name_set("SLZB-06MU bridge");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_zigbee", "_tcp", cfg.tcp_port, NULL, 0);

    if (cfg.ntp_server[0]) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, cfg.ntp_server);
        esp_sntp_init();
        setenv("TZ", cfg.timezone, 1);
        tzset();
    }

    xTaskCreate(fallback_task, "netfb", 3072, NULL, 3, NULL);
}
