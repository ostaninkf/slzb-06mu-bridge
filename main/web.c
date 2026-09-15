/* Веб-интерфейс: состояние, настройки, обновление прошивки моста и радио.
 * Всё, что раньше жило в штатной панели и было нужно для эксплуатации. */
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
#include "settings.h"
#include "net.h"
#include "ncpflash.h"
#include "web.h"
#include "safety.h"

static const char *TAG = "web";

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>SLZB-06MU bridge</title><style>"
"body{font:14px/1.5 system-ui;margin:0;padding:1.5rem;max-width:44rem}"
"h1{font-size:1.3rem;margin:0 0 1rem}h2{font-size:1rem;margin:1.5rem 0 .5rem}"
"table{border-collapse:collapse;width:100%}td{padding:.3rem .5rem;border-bottom:1px solid #ddd}"
"td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
"label{display:flex;justify-content:space-between;gap:1rem;padding:.3rem 0;align-items:center}"
"input,select{font:inherit;padding:.25rem .4rem;min-width:12rem}input[type=checkbox]{min-width:auto}"
"button{font:inherit;padding:.45rem .9rem;margin:.3rem .4rem .3rem 0}"
"#msg{position:sticky;top:0;background:#ffd;padding:.4rem;display:none}"
"#lang{float:right;min-width:auto}"
"@media(prefers-color-scheme:dark){body{background:#111;color:#eee}td{border-color:#333}"
"input,select,button{background:#222;color:#eee;border:1px solid #444}#msg{background:#332}}</style>"
"<select id=lang onchange=\"setLang(this.value)\"><option value=ru>Русский</option>"
"<option value=en>English</option></select>"
"<div id=msg></div><h1>SLZB-06MU</h1>"
"<h2 id=h_state></h2><table id=st></table>"
"<h2 id=h_cfg></h2><form id=f></form>"
"<button id=b_save onclick=\"save()\"></button>"
"<button id=b_ncp onclick=\"post('/ncp/reset')\"></button>"
"<button id=b_reboot onclick=\"post('/reboot')\"></button>"
"<h2 id=h_upd></h2>"
"<p><span id=l_fw></span> <input type=file id=fw accept=.bin> <button id=b_fw onclick=\"up('/update','fw')\"></button>"
"<br><span id=l_gbl></span> <input type=file id=gbl accept=.gbl> <button id=b_gbl onclick=\"up('/ncp/update','gbl')\"></button></p>"
"<script>"
"const L={ru:{state:'Состояние',cfg:'Настройки',upd:'Обновление',save:'Сохранить и перезагрузить',"
"ncp:'Сбросить радио',reboot:'Перезагрузить мост',fw:'Прошивка моста:',gbl:'Прошивка радио (.gbl):',"
"send:'Залить',saved:'сохранено, перезагружаюсь',failed:'не сохранилось: ',sent:'команда отправлена',"
"nofile:'файл не выбран',sending:'заливаю ',bytes:' байт…',"
"uptime:'аптайм, с',ip:'адрес',link:'канал',clients:'клиентов на радио',ncp_to_net:'байт от радио',"
"net_to_ncp:'байт к радио',ring_used:'в буфере',ring_full:'переполнений буфера',connects:'подключений',"
"disconnects:'обрывов',dropped_no_client:'отброшено без клиента',ncp_resets:'сбросов радио',"
"temp:'температура, °C',heap:'свободная память',min_heap:'минимум памяти',version:'версия',"
"image:'образ',crashes:'аварий подряд',"
"hostname:'Имя в сети',eth_dhcp:'Ethernet: DHCP',eth_ip:'IP',eth_mask:'Маска',eth_gw:'Шлюз',"
"dns1:'DNS 1',dns2:'DNS 2',mac_override:'Задать MAC вручную',mac:'MAC',"
"wifi_enabled:'Wi-Fi как резерв',wifi_ssid:'Wi-Fi сеть',wifi_pass:'Wi-Fi пароль',"
"ap_fallback:'Точка доступа, если сети нет',ncp_baud:'Скорость UART радио',flow:'Управление потоком',"
"tcp_port:'TCP-порт радио',ncp_route:'Куда отдавать радио',leds_enabled:'Светодиоды',"
"night_mode:'Ночной режим',night_from:'Гасить с (час)',night_to:'Гасить до (часа)',"
"mqtt_enabled:'MQTT',mqtt_host:'MQTT сервер',mqtt_port:'MQTT порт',mqtt_user:'MQTT логин',"
"mqtt_pass:'MQTT пароль',mqtt_base:'MQTT базовый топик',syslog_enabled:'Syslog',"
"syslog_host:'Syslog сервер',syslog_port:'Syslog порт',ntp_server:'NTP сервер',timezone:'Часовой пояс',"
"f0:'выключено',f1:'RTS (рекомендуется)',f2:'RTS+CTS',r0:'в сеть по TCP',r1:'в USB',"
"set:'задан',unset:'пусто'},"
"en:{state:'Status',cfg:'Settings',upd:'Update',save:'Save and reboot',"
"ncp:'Reset radio',reboot:'Reboot bridge',fw:'Bridge firmware:',gbl:'Radio firmware (.gbl):',"
"send:'Upload',saved:'saved, rebooting',failed:'not saved: ',sent:'command sent',"
"nofile:'no file selected',sending:'uploading ',bytes:' bytes…',"
"uptime:'uptime, s',ip:'address',link:'uplink',clients:'clients on radio',ncp_to_net:'bytes from radio',"
"net_to_ncp:'bytes to radio',ring_used:'in buffer',ring_full:'buffer overflows',connects:'connections',"
"disconnects:'disconnects',dropped_no_client:'dropped, no client',ncp_resets:'radio resets',"
"temp:'chip temperature, °C',heap:'free heap',min_heap:'minimum heap',version:'version',"
"image:'image',crashes:'crashes in a row',"
"hostname:'Hostname',eth_dhcp:'Ethernet: DHCP',eth_ip:'IP',eth_mask:'Netmask',eth_gw:'Gateway',"
"dns1:'DNS 1',dns2:'DNS 2',mac_override:'Set MAC manually',mac:'MAC',"
"wifi_enabled:'Wi-Fi as fallback',wifi_ssid:'Wi-Fi network',wifi_pass:'Wi-Fi password',"
"ap_fallback:'Access point when no network',ncp_baud:'Radio UART baud rate',flow:'Flow control',"
"tcp_port:'Radio TCP port',ncp_route:'Where to route the radio',leds_enabled:'LEDs',"
"night_mode:'Night mode',night_from:'Dim from (hour)',night_to:'Dim until (hour)',"
"mqtt_enabled:'MQTT',mqtt_host:'MQTT host',mqtt_port:'MQTT port',mqtt_user:'MQTT user',"
"mqtt_pass:'MQTT password',mqtt_base:'MQTT base topic',syslog_enabled:'Syslog',"
"syslog_host:'Syslog host',syslog_port:'Syslog port',ntp_server:'NTP server',timezone:'Time zone',"
"f0:'off',f1:'RTS (recommended)',f2:'RTS+CTS',r0:'to network over TCP',r1:'to USB',"
"set:'set',unset:'empty'}};"
"const FIELDS=['hostname','eth_dhcp','eth_ip','eth_mask','eth_gw','dns1','dns2','mac_override','mac',"
"'wifi_enabled','wifi_ssid','wifi_pass','ap_fallback','ncp_baud','flow','tcp_port','ncp_route',"
"'leds_enabled','night_mode','night_from','night_to','mqtt_enabled','mqtt_host','mqtt_port',"
"'mqtt_user','mqtt_pass','mqtt_base','syslog_enabled','syslog_host','syslog_port','ntp_server','timezone'];"
"const SEL={flow:['f0','f1','f2'],ncp_route:['r0','r1']};"
"let lang=localStorage.getItem('lang')||(navigator.language.startsWith('ru')?'ru':'en');"
"let cur={};"
"function t(k){return L[lang][k]||k}"
"function setLang(v){lang=v;localStorage.setItem('lang',v);paint();load()}"
"function paint(){document.getElementById('lang').value=lang;"
"document.getElementById('h_state').textContent=t('state');"
"document.getElementById('h_cfg').textContent=t('cfg');"
"document.getElementById('h_upd').textContent=t('upd');"
"document.getElementById('b_save').textContent=t('save');"
"document.getElementById('b_ncp').textContent=t('ncp');"
"document.getElementById('b_reboot').textContent=t('reboot');"
"document.getElementById('l_fw').textContent=t('fw');"
"document.getElementById('l_gbl').textContent=t('gbl');"
"document.getElementById('b_fw').textContent=t('send');"
"document.getElementById('b_gbl').textContent=t('send')}"
"function note(x){const m=document.getElementById('msg');m.textContent=x;m.style.display='block';"
"setTimeout(()=>m.style.display='none',6000)}"
"async function st(){const r=await(await fetch('/status')).json();"
"document.getElementById('st').innerHTML=Object.entries(r).map(([k,v])=>"
"`<tr><td>${t(k)}</td><td>${v}</td></tr>`).join('')}"
"async function load(){cur=await(await fetch('/settings')).json();"
"document.getElementById('f').innerHTML=FIELDS.map(k=>{"
"if(k.endsWith('_pass'))return `<label>${t(k)}<input id=i_${k} type=password placeholder='${cur[k+'_set']?t('set'):t('unset')}'></label>`;"
"if(SEL[k])return `<label>${t(k)}<select id=i_${k}>`+SEL[k].map((c,i)=>"
"`<option value=${i} ${cur[k]==i?'selected':''}>${t(c)}</option>`).join('')+'</select></label>';"
"if(typeof cur[k]==='boolean')return `<label>${t(k)}<input id=i_${k} type=checkbox ${cur[k]?'checked':''}></label>`;"
"if(typeof cur[k]==='number')return `<label>${t(k)}<input id=i_${k} type=number value='${cur[k]}'></label>`;"
"return `<label>${t(k)}<input id=i_${k} value='${(cur[k]!==undefined?cur[k]:'')}'></label>`}).join('')}"
"async function save(){const o={};for(const k of FIELDS){const e=document.getElementById('i_'+k);"
"if(!e)continue;if(e.type==='checkbox')o[k]=e.checked;"
"else if(e.type==='password'){if(e.value)o[k]=e.value}"
"else if(e.type==='number'||SEL[k])o[k]=Number(e.value);else o[k]=e.value}"
"const r=await fetch('/settings',{method:'POST',body:JSON.stringify(o)});"
"note(r.ok?t('saved'):t('failed')+await r.text());"
"if(r.ok)setTimeout(()=>location.reload(),9000)}"
"async function post(u){await fetch(u,{method:'POST'});note(t('sent'))}"
"async function up(url,id){const f=document.getElementById(id).files[0];if(!f)return note(t('nofile'));"
"note(t('sending')+f.name+', '+f.size+t('bytes'));"
"const r=await fetch(url,{method:'POST',body:f});note(await r.text())}"
"paint();st();load();setInterval(st,2000);</script>";

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
    char buf[800];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime\":%llu,\"ip\":\"%s\",\"link\":\"%s\",\"clients\":%d,"
        "\"ncp_to_net\":%llu,\"net_to_ncp\":%llu,\"ring_used\":%u,\"ring_full\":%u,"
        "\"connects\":%u,\"disconnects\":%u,\"dropped_no_client\":%llu,\"ncp_resets\":%u,"
        "\"temp\":%.1f,\"heap\":%u,\"min_heap\":%u,\"version\":\"%s %s\","
        "\"image\":\"%s\",\"crashes\":%u}",
        (unsigned long long)(esp_timer_get_time() / 1000000), net_ip(),
        net_eth_link() ? "витая пара" : net_wifi_link() ? "Wi-Fi" : net_ap_up() ? "точка доступа" : "нет",
        s.connects > s.disconnects ? 1 : 0,
        (unsigned long long)s.ncp_to_net, (unsigned long long)s.net_to_ncp,
        (unsigned)s.ring_used, (unsigned)s.ring_full, (unsigned)s.connects,
        (unsigned)s.disconnects, (unsigned long long)s.dropped_no_client,
        (unsigned)s.ncp_resets, bridge_chip_temp(),
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        d->version, d->date,
        safety_on_trial() ? "на испытательном сроке" : "признан рабочим",
        safety_crashes());
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n);
}

static void reboot_task(void *a) { vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }

/* httpd_req_recv отдаёт HTTPD_SOCK_ERR_TIMEOUT, если за recv_wait_timeout не
 * пришло ни байта. Это не обрыв, а обычная пауза: обработчики же считали любой
 * возврат <= 0 фатальным, и заливка прошивки по OTA обрывалась на середине
 * («обрыв приёма», 15.09.2026, 09:07). Ждём молчания суммарно до минуты. */
static int recv_chunk(httpd_req_t *r, char *buf, int len)
{
    for (int tries = 0; tries < 12; tries++) {
        int n = httpd_req_recv(r, buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) return n;
    }
    return HTTPD_SOCK_ERR_TIMEOUT;
}

static esp_err_t h_settings_get(httpd_req_t *r)
{
    char buf[1400];
    int n = settings_to_json(buf, sizeof(buf));
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n);
}

static esp_err_t h_settings_post(httpd_req_t *r)
{
    if (r->content_len > 3000) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "слишком длинно");
    char body[3001];
    int got = 0;
    while (got < r->content_len) {
        int n = recv_chunk(r, body + got, r->content_len - got);
        if (n <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "обрыв приёма");
        got += n;
    }
    body[got] = 0;

    if (settings_from_json(body) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "не разобрал настройки");

    httpd_resp_send(r, "ok", 2);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);   /* применяем перезагрузкой */
    return ESP_OK;
}

static esp_err_t h_ncp_reset(httpd_req_t *r) { ncp_reset(); return httpd_resp_send(r, "радио сброшено", HTTPD_RESP_USE_STRLEN); }

static esp_err_t h_reboot(httpd_req_t *r)
{
    httpd_resp_send(r, "перезагружаюсь", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Обновление прошивки моста: тело запроса — образ, как его собирает idf.py. */
static esp_err_t h_update(httpd_req_t *r)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "нет OTA-раздела");

    esp_ota_handle_t h;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin");

    ESP_LOGW(TAG, "приём прошивки моста (%d байт) в раздел %s", r->content_len, part->label);
    char buf[2048];
    int left = r->content_len;
    while (left > 0) {
        int n = recv_chunk(r, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n <= 0) { esp_ota_abort(h); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "обрыв приёма"); }
        if (esp_ota_write(h, buf, n) != ESP_OK) { esp_ota_abort(h); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "запись не удалась"); }
        left -= n;
    }
    if (esp_ota_end(h) != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "образ не прошёл проверку");
    if (esp_ota_set_boot_partition(part) != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "не переключился загрузочный раздел");

    httpd_resp_send(r, "прошивка принята, перезагружаюсь", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Обновление прошивки радиомодуля: тело — .gbl, уходит в загрузчик по XMODEM. */
static esp_err_t h_ncp_update(httpd_req_t *r)
{
    if (ncp_flash_begin() != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "загрузчик радио не отозвался");

    ESP_LOGW(TAG, "приём прошивки радио, %d байт", r->content_len);
    char buf[1024];
    int left = r->content_len;
    while (left > 0) {
        int n = recv_chunk(r, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n <= 0) { ncp_flash_abort(); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "обрыв приёма"); }
        if (ncp_flash_feed((const uint8_t *)buf, n) != ESP_OK)
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "радио не приняло блок");
        left -= n;
    }
    esp_err_t err = ncp_flash_end();
    char msg[96];
    snprintf(msg, sizeof(msg), "радио прошито: %u байт, %s",
             (unsigned)ncp_flash_sent(), err == ESP_OK ? "подтверждено" : "без подтверждения");
    return httpd_resp_send(r, msg, HTTPD_RESP_USE_STRLEN);
}

void web_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.lru_purge_enable = true;
    c.stack_size = 8192;
    c.max_uri_handlers = 10;
    c.recv_wait_timeout = 5;
    c.send_wait_timeout = 5;
    c.max_open_sockets = 5;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &c) != ESP_OK) { ESP_LOGE(TAG, "веб-сервер не поднялся"); return; }

    const httpd_uri_t u[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = h_root },
        { .uri = "/status",     .method = HTTP_GET,  .handler = h_status },
        { .uri = "/settings",   .method = HTTP_GET,  .handler = h_settings_get },
        { .uri = "/settings",   .method = HTTP_POST, .handler = h_settings_post },
        { .uri = "/ncp/reset",  .method = HTTP_POST, .handler = h_ncp_reset },
        { .uri = "/ncp/update", .method = HTTP_POST, .handler = h_ncp_update },
        { .uri = "/reboot",     .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/update",     .method = HTTP_POST, .handler = h_update },
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(srv, &u[i]);
    ESP_LOGI(TAG, "веб-интерфейс на :80");
}
