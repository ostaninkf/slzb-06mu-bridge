/* Настройки моста. Хранятся в NVS, правятся через веб; в коде нет ни одного
 * зашитого адреса, порта или пароля — только заводские значения по умолчанию. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define S_STR 64

typedef enum { FLOW_NONE = 0, FLOW_RTS = 1, FLOW_RTS_CTS = 2 } flow_mode_t;
typedef enum { NCP_TO_LAN = 0, NCP_TO_USB = 1 } ncp_route_t;

typedef struct {
    /* сеть */
    char     hostname[S_STR];
    bool     eth_dhcp;
    char     eth_ip[S_STR], eth_mask[S_STR], eth_gw[S_STR];
    char     dns1[S_STR], dns2[S_STR];
    bool     mac_override;            /* иначе адрес выводится из eFuse */
    uint8_t  mac[6];

    /* Wi-Fi: резервный канал, поднимается когда нет линка по витой паре */
    bool     wifi_enabled;
    char     wifi_ssid[S_STR], wifi_pass[S_STR];
    bool     ap_fallback;             /* точка доступа, если сети нет вовсе */
    char     ap_pass[S_STR];

    /* радио и мост */
    uint32_t ncp_baud;
    uint8_t  flow;                    /* flow_mode_t */
    uint16_t tcp_port;
    uint8_t  ncp_route;               /* ncp_route_t: в сеть или в USB */

    /* индикация */
    bool     leds_enabled;
    bool     night_mode;              /* гасить в ночные часы */
    uint8_t  night_from, night_to;    /* часы, местное время */

    /* MQTT: прошивка сама публикует метрики и HA-discovery */
    bool     mqtt_enabled;
    char     mqtt_host[S_STR], mqtt_user[S_STR], mqtt_pass[S_STR], mqtt_base[S_STR];
    uint16_t mqtt_port;

    /* журнал на удалённый syslog */
    bool     syslog_enabled;
    char     syslog_host[S_STR];
    uint16_t syslog_port;

    /* время для ночного режима */
    char     ntp_server[S_STR];
    char     timezone[S_STR];
} settings_t;

extern settings_t cfg;

void settings_load(void);      /* читает NVS, недостающее берёт из умолчаний */
esp_err_t settings_save(void); /* пишет всё разом */
void settings_defaults(settings_t *s);
int  settings_to_json(char *buf, size_t len);       /* пароли не отдаются */
esp_err_t settings_from_json(const char *json);     /* правит только присланные поля */
