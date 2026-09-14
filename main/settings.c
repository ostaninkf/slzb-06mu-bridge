#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "settings.h"
#include "board.h"

static const char *TAG = "cfg";
static const char *NS = "bridge";

settings_t cfg;

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->hostname, S_STR, "slzb-06mu");
    s->eth_dhcp = true;
    s->mac_override = false;

    s->wifi_enabled = false;
    s->ap_fallback = true;
    snprintf(s->ap_pass, S_STR, "slzb-setup");   /* меняется при первом заходе */

    s->ncp_baud = NCP_BAUD;
    s->flow = FLOW_RTS;          /* то, чего не хватало штатной прошивке */
    s->tcp_port = NCP_TCP_PORT;
    s->ncp_route = NCP_TO_LAN;

    s->leds_enabled = true;
    s->night_mode = false;
    s->night_from = 23; s->night_to = 7;

    s->mqtt_enabled = false;
    s->mqtt_port = 1883;
    snprintf(s->mqtt_base, S_STR, "slzb06mu");

    s->syslog_enabled = false;
    s->syslog_port = 514;

    snprintf(s->ntp_server, S_STR, "pool.ntp.org");
    snprintf(s->timezone, S_STR, "UTC0");
}

#define GETS(key, field) do { size_t l = S_STR; nvs_get_str(h, key, cfg.field, &l); } while (0)
#define GETU8(key, field)  do { uint8_t v;  if (nvs_get_u8(h, key, &v)  == ESP_OK) cfg.field = v; } while (0)
#define GETU16(key, field) do { uint16_t v; if (nvs_get_u16(h, key, &v) == ESP_OK) cfg.field = v; } while (0)
#define GETU32(key, field) do { uint32_t v; if (nvs_get_u32(h, key, &v) == ESP_OK) cfg.field = v; } while (0)

void settings_load(void)
{
    settings_defaults(&cfg);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "настроек в NVS нет, беру заводские");
        return;
    }
    GETS("hostname", hostname);
    GETU8("eth_dhcp", eth_dhcp);
    GETS("eth_ip", eth_ip); GETS("eth_mask", eth_mask); GETS("eth_gw", eth_gw);
    GETS("dns1", dns1); GETS("dns2", dns2);
    GETU8("mac_ovr", mac_override);
    size_t ml = 6; nvs_get_blob(h, "mac", cfg.mac, &ml);

    GETU8("wifi_en", wifi_enabled);
    GETS("wifi_ssid", wifi_ssid); GETS("wifi_pass", wifi_pass);
    GETU8("ap_fb", ap_fallback); GETS("ap_pass", ap_pass);

    GETU32("ncp_baud", ncp_baud);
    GETU8("flow", flow);
    GETU16("tcp_port", tcp_port);
    GETU8("ncp_route", ncp_route);

    GETU8("leds_en", leds_enabled);
    GETU8("night", night_mode);
    GETU8("night_from", night_from); GETU8("night_to", night_to);

    GETU8("mqtt_en", mqtt_enabled);
    GETS("mqtt_host", mqtt_host); GETS("mqtt_user", mqtt_user);
    GETS("mqtt_pass", mqtt_pass); GETS("mqtt_base", mqtt_base);
    GETU16("mqtt_port", mqtt_port);

    GETU8("syslog_en", syslog_enabled);
    GETS("syslog_host", syslog_host);
    GETU16("syslog_port", syslog_port);

    GETS("ntp", ntp_server); GETS("tz", timezone);
    nvs_close(h);
    ESP_LOGI(TAG, "настройки прочитаны: %s, порт %u, поток %u, baud %lu",
             cfg.hostname, cfg.tcp_port, cfg.flow, (unsigned long)cfg.ncp_baud);
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "hostname", cfg.hostname);
    nvs_set_u8(h, "eth_dhcp", cfg.eth_dhcp);
    nvs_set_str(h, "eth_ip", cfg.eth_ip);
    nvs_set_str(h, "eth_mask", cfg.eth_mask);
    nvs_set_str(h, "eth_gw", cfg.eth_gw);
    nvs_set_str(h, "dns1", cfg.dns1);
    nvs_set_str(h, "dns2", cfg.dns2);
    nvs_set_u8(h, "mac_ovr", cfg.mac_override);
    nvs_set_blob(h, "mac", cfg.mac, 6);

    nvs_set_u8(h, "wifi_en", cfg.wifi_enabled);
    nvs_set_str(h, "wifi_ssid", cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass", cfg.wifi_pass);
    nvs_set_u8(h, "ap_fb", cfg.ap_fallback);
    nvs_set_str(h, "ap_pass", cfg.ap_pass);

    nvs_set_u32(h, "ncp_baud", cfg.ncp_baud);
    nvs_set_u8(h, "flow", cfg.flow);
    nvs_set_u16(h, "tcp_port", cfg.tcp_port);
    nvs_set_u8(h, "ncp_route", cfg.ncp_route);

    nvs_set_u8(h, "leds_en", cfg.leds_enabled);
    nvs_set_u8(h, "night", cfg.night_mode);
    nvs_set_u8(h, "night_from", cfg.night_from);
    nvs_set_u8(h, "night_to", cfg.night_to);

    nvs_set_u8(h, "mqtt_en", cfg.mqtt_enabled);
    nvs_set_str(h, "mqtt_host", cfg.mqtt_host);
    nvs_set_str(h, "mqtt_user", cfg.mqtt_user);
    nvs_set_str(h, "mqtt_pass", cfg.mqtt_pass);
    nvs_set_str(h, "mqtt_base", cfg.mqtt_base);
    nvs_set_u16(h, "mqtt_port", cfg.mqtt_port);

    nvs_set_u8(h, "syslog_en", cfg.syslog_enabled);
    nvs_set_str(h, "syslog_host", cfg.syslog_host);
    nvs_set_u16(h, "syslog_port", cfg.syslog_port);

    nvs_set_str(h, "ntp", cfg.ntp_server);
    nvs_set_str(h, "tz", cfg.timezone);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "настройки сохранены");
    return err;
}

int settings_to_json(char *buf, size_t len)
{
    /* Пароли наружу не отдаются: вместо них признак «задан или нет». */
    return snprintf(buf, len,
        "{\"hostname\":\"%s\",\"eth_dhcp\":%s,\"eth_ip\":\"%s\",\"eth_mask\":\"%s\","
        "\"eth_gw\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\",\"mac_override\":%s,"
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"wifi_enabled\":%s,\"wifi_ssid\":\"%s\",\"wifi_pass_set\":%s,"
        "\"ap_fallback\":%s,\"ncp_baud\":%lu,\"flow\":%u,\"tcp_port\":%u,\"ncp_route\":%u,"
        "\"leds_enabled\":%s,\"night_mode\":%s,\"night_from\":%u,\"night_to\":%u,"
        "\"mqtt_enabled\":%s,\"mqtt_host\":\"%s\",\"mqtt_port\":%u,\"mqtt_user\":\"%s\","
        "\"mqtt_pass_set\":%s,\"mqtt_base\":\"%s\","
        "\"syslog_enabled\":%s,\"syslog_host\":\"%s\",\"syslog_port\":%u,"
        "\"ntp_server\":\"%s\",\"timezone\":\"%s\"}",
        cfg.hostname, cfg.eth_dhcp ? "true" : "false", cfg.eth_ip, cfg.eth_mask,
        cfg.eth_gw, cfg.dns1, cfg.dns2, cfg.mac_override ? "true" : "false",
        cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5],
        cfg.wifi_enabled ? "true" : "false", cfg.wifi_ssid,
        cfg.wifi_pass[0] ? "true" : "false", cfg.ap_fallback ? "true" : "false",
        (unsigned long)cfg.ncp_baud, cfg.flow, cfg.tcp_port, cfg.ncp_route,
        cfg.leds_enabled ? "true" : "false", cfg.night_mode ? "true" : "false",
        cfg.night_from, cfg.night_to,
        cfg.mqtt_enabled ? "true" : "false", cfg.mqtt_host, cfg.mqtt_port,
        cfg.mqtt_user, cfg.mqtt_pass[0] ? "true" : "false", cfg.mqtt_base,
        cfg.syslog_enabled ? "true" : "false", cfg.syslog_host, cfg.syslog_port,
        cfg.ntp_server, cfg.timezone);
}

static void js_str(const cJSON *o, const char *k, char *dst)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(i) && i->valuestring) { strncpy(dst, i->valuestring, S_STR - 1); dst[S_STR - 1] = 0; }
}
static void js_bool(const cJSON *o, const char *k, bool *dst)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(i)) *dst = cJSON_IsTrue(i);
}
static void js_num(const cJSON *o, const char *k, long min, long max, long *dst)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(i) && i->valuedouble >= min && i->valuedouble <= max) *dst = (long)i->valuedouble;
}

esp_err_t settings_from_json(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) return ESP_ERR_INVALID_ARG;

    long v;
    js_str(o, "hostname", cfg.hostname);
    js_bool(o, "eth_dhcp", &cfg.eth_dhcp);
    js_str(o, "eth_ip", cfg.eth_ip); js_str(o, "eth_mask", cfg.eth_mask);
    js_str(o, "eth_gw", cfg.eth_gw); js_str(o, "dns1", cfg.dns1); js_str(o, "dns2", cfg.dns2);
    js_bool(o, "mac_override", &cfg.mac_override);

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "mac");
    if (cJSON_IsString(m) && m->valuestring) {
        unsigned b[6];
        if (sscanf(m->valuestring, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6)
            for (int i = 0; i < 6; i++) cfg.mac[i] = (uint8_t)b[i];
    }

    js_bool(o, "wifi_enabled", &cfg.wifi_enabled);
    js_str(o, "wifi_ssid", cfg.wifi_ssid);
    js_str(o, "wifi_pass", cfg.wifi_pass);      /* пустая строка не приходит — поле не трогается */
    js_bool(o, "ap_fallback", &cfg.ap_fallback);
    js_str(o, "ap_pass", cfg.ap_pass);

    v = cfg.ncp_baud; js_num(o, "ncp_baud", 9600, 1000000, &v); cfg.ncp_baud = v;
    v = cfg.flow;     js_num(o, "flow", 0, 2, &v);              cfg.flow = v;
    v = cfg.tcp_port; js_num(o, "tcp_port", 1, 65535, &v);      cfg.tcp_port = v;
    v = cfg.ncp_route; js_num(o, "ncp_route", 0, 1, &v);        cfg.ncp_route = v;

    js_bool(o, "leds_enabled", &cfg.leds_enabled);
    js_bool(o, "night_mode", &cfg.night_mode);
    v = cfg.night_from; js_num(o, "night_from", 0, 23, &v); cfg.night_from = v;
    v = cfg.night_to;   js_num(o, "night_to", 0, 23, &v);   cfg.night_to = v;

    js_bool(o, "mqtt_enabled", &cfg.mqtt_enabled);
    js_str(o, "mqtt_host", cfg.mqtt_host);
    js_str(o, "mqtt_user", cfg.mqtt_user);
    js_str(o, "mqtt_pass", cfg.mqtt_pass);
    js_str(o, "mqtt_base", cfg.mqtt_base);
    v = cfg.mqtt_port; js_num(o, "mqtt_port", 1, 65535, &v); cfg.mqtt_port = v;

    js_bool(o, "syslog_enabled", &cfg.syslog_enabled);
    js_str(o, "syslog_host", cfg.syslog_host);
    v = cfg.syslog_port; js_num(o, "syslog_port", 1, 65535, &v); cfg.syslog_port = v;

    js_str(o, "ntp_server", cfg.ntp_server);
    js_str(o, "timezone", cfg.timezone);

    cJSON_Delete(o);
    return settings_save();
}
