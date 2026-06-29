// wifi_web.c — Wi-Fi (AP+STA) + serveur HTTP minimal : UNE page pour configurer le réseau
// station (SSID / mot de passe / activation), persisté en NVS. Démarre aussi le NTP une fois
// connecté. Inspiré du firmware du kart, réduit au strict nécessaire.
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_web.h"

static const char* TAG = "wifi";

#define AP_SSID "LCD-Test"
#define AP_PASS "test12345"
#define NVS_NS "wifi"
#define TZ_STR "EST5EDT,M3.2.0,M11.1.0"   // heure de l'Est (Québec)

static char m_sta_ip[16] = "0.0.0.0";
static volatile bool m_sta_connected = false;
static char m_sta_ssid[33] = "";
static bool m_sntp_started = false;

// ───────────────────────── NVS ─────────────────────────
static void creds_save(const char* ssid, const char* pass)
{
    nvs_handle_t h;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READWRITE, &h)) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

static bool creds_load(char* ssid, size_t ssid_sz, char* pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READONLY, &h)) return false;
    bool ok = (ESP_OK == nvs_get_str(h, "ssid", ssid, &ssid_sz)) && ssid[0];
    if (ESP_OK != nvs_get_str(h, "pass", pass, &pass_sz)) pass[0] = '\0';
    nvs_close(h);
    return ok;
}

static bool sta_is_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (ESP_OK == nvs_open(NVS_NS, NVS_READONLY, &h))
    {
        nvs_get_u8(h, "sta_en", &v);
        nvs_close(h);
    }
    return 0 != v;
}

static void sta_set_enabled(bool en)
{
    nvs_handle_t h;
    if (ESP_OK == nvs_open(NVS_NS, NVS_READWRITE, &h))
    {
        nvs_set_u8(h, "sta_en", en ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ───────────────────────── NTP ─────────────────────────
static void sntp_start_once(void)
{
    if (m_sntp_started) return;
    m_sntp_started = true;
    setenv("TZ", TZ_STR, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "NTP démarré (fuseau %s)", TZ_STR);
}

// ───────────────────────── STA ─────────────────────────
static void sta_connect_saved(void)
{
    char ssid[33], pass[64];
    if (!sta_is_enabled() || !creds_load(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        return;   // désactivé ou pas d'identifiants → on reste en AP seul
    }
    wifi_config_t sta = {0};
    strncpy((char*)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char*)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    strncpy(m_sta_ssid, ssid, sizeof(m_sta_ssid) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_connect();
    ESP_LOGI(TAG, "Connexion station à « %s »", ssid);
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (WIFI_EVENT == base && WIFI_EVENT_STA_DISCONNECTED == id)
    {
        m_sta_connected = false;
        strcpy(m_sta_ip, "0.0.0.0");
        if (sta_is_enabled())
        {
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_wifi_connect();   // reconnexion auto tant que la station est activée
        }
    }
    else if (IP_EVENT == base && IP_EVENT_STA_GOT_IP == id)
    {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        snprintf(m_sta_ip, sizeof(m_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        m_sta_connected = true;
        ESP_LOGI(TAG, "Station connectée, IP %s", m_sta_ip);
        sntp_start_once();
    }
}

// ───────────────────────── Page web ─────────────────────────
static const char PAGE[] =
    "<!DOCTYPE html><html lang=fr><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Config Wi-Fi</title><style>"
    "body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:420px;margin:24px auto;padding:0 16px}"
    "h1{font-size:20px}label{display:block;margin:12px 0 4px;color:#aaa}"
    "input[type=text],input[type=password]{width:100%;padding:10px;border-radius:8px;border:1px solid #444;background:#1c1c1c;color:#eee;box-sizing:border-box}"
    ".chk{display:flex;align-items:center;gap:8px;margin:14px 0;color:#eee}"
    "button{margin-top:16px;width:100%;padding:12px;border:0;border-radius:8px;background:#3cf;color:#012;font-weight:700;font-size:15px}"
    ".st{margin:12px 0;padding:10px 12px;border-radius:8px;background:#1c1c1c;border:1px solid #333}"
    ".ok{color:#6f6}.no{color:#f88}</style></head><body>"
    "<h1>&#128246; Configuration Wi-Fi</h1>"
    "<div class=st id=st>Etat : &hellip;</div>"
    "<form method=POST action=/save>"
    "<label>Reseau (SSID)</label><input type=text name=ssid maxlength=32>"
    "<label>Mot de passe</label><input type=password name=pass maxlength=63>"
    "<div class=chk><input type=checkbox id=staen name=staen><label for=staen style='margin:0'>Activer le Wi-Fi station</label></div>"
    "<button type=submit>Enregistrer</button></form>"
    "<script>"
    "function u(){fetch('/status').then(r=>r.json()).then(s=>{"
    "document.getElementById('st').innerHTML=s.sta"
    "?('Etat : <span class=ok>connecte</span> a '+s.ssid+' &mdash; IP '+s.ip)"
    ":('Etat : <span class=no>'+(s.en?'connexion&hellip;':'station desactivee')+'</span>');"
    "var c=document.getElementById('staen');if(document.activeElement!==c)c.checked=s.en;});}"
    "u();setInterval(u,2000);"
    "</script></body></html>";

static esp_err_t root_get(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static bool form_field(const char* body, const char* key, char* out, size_t out_sz)
{
    out[0] = '\0';
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '&' && o + 1 < out_sz)
    {
        if ('%' == *p && p[1] && p[2]) { char hex[3] = {p[1], p[2], 0}; out[o++] = (char)strtol(hex, NULL, 16); p += 3; }
        else if ('+' == *p) { out[o++] = ' '; ++p; }
        else { out[o++] = *p++; }
    }
    out[o] = '\0';
    return true;
}

static esp_err_t save_post(httpd_req_t* req)
{
    char body[256] = {0};
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return ESP_FAIL;
    body[got] = '\0';

    char ssid[33] = "", pass[64] = "", tmp[8] = "";
    bool has_ssid = form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    const bool enable = form_field(body, "staen", tmp, sizeof(tmp));   // case cochée = champ présent

    if (has_ssid && ssid[0]) creds_save(ssid, pass);   // ne pas écraser les identifiants si SSID vide
    sta_set_enabled(enable);

    if (enable) { esp_wifi_disconnect(); sta_connect_saved(); }
    else        { esp_wifi_disconnect(); ESP_LOGI(TAG, "Station désactivée (identifiants conservés)"); }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_get(httpd_req_t* req)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"sta\":%s,\"en\":%s,\"ip\":\"%s\",\"ssid\":\"%s\"}",
             m_sta_connected ? "true" : "false", sta_is_enabled() ? "true" : "false",
             m_sta_ip, m_sta_ssid);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t srv = NULL;
    if (ESP_OK != httpd_start(&srv, &cfg)) return;
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = status_get};
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &save);
    httpd_register_uri_handler(srv, &status);
    ESP_LOGI(TAG, "Serveur web prêt → http://192.168.4.1");
}

void wifi_web_start(void)
{
    if (ESP_OK != nvs_flash_init()) { nvs_flash_erase(); nvs_flash_init(); }

    // (La migration unique « activer la station par défaut » a été retirée : l'état est
    //  désormais piloté par la case à cocher de la page web, persistée en NVS.)

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    wifi_config_t ap = {0};
    strcpy((char*)ap.ap.ssid, AP_SSID);
    strcpy((char*)ap.ap.password, AP_PASS);
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power-save OFF : évite les réveils radio périodiques qui perturbent le DMA RGB
    // (atténue les glitchs d'affichage dus à la contention PSRAM avec le Wi-Fi).
    esp_wifi_set_ps(WIFI_PS_NONE);

    http_start();
    sta_connect_saved();
    ESP_LOGI(TAG, "Point d'accès « %s » actif", AP_SSID);
}
