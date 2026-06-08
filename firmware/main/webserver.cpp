// webserver.cpp — SoftAP + serveur HTTP/WebSocket pour régler la config en direct.
//
// Connexion : Wi-Fi « Kart-Config » (mot de passe « kart12345 »), puis
// ouvrir http://192.168.4.1 dans un navigateur.
//
// La page (HTML/CSS) vient de main/assets/ (EMBED_TXTFILES). Comm par WebSocket /ws :
//   le client POLL à 4 Hz et le serveur répond de façon synchrone :
//     {"type":"status"}     → {"type":"status",...}
//     {"type":"get"}        → {"type":"config",...}
//     {"type":"set",...}    → applique + {"type":"config",...}
//     {"type":"calibrate"}  → déclenche la calibration
//     {"type":"reboot"}     → redémarre l'ESP32 (désarmé)
// JSON « maison » (clé/valeur plates) — pas de dépendance externe.
#include "webserver.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "config.hpp"
#include "ringbuffer.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "web";

// Fichiers embarqués (EMBED_TXTFILES dans main/CMakeLists.txt) — terminés par nul.
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char style_css_start[]  asm("_binary_style_css_start");

namespace
{
constexpr char AP_SSID[]  = "Kart-Config";
constexpr char AP_PASS[]  = "kart12345";   // ≥ 8 caractères (sinon AP ouvert)
constexpr int  AP_CHANNEL = 1;
constexpr int  AP_MAX_CONN = 4;
constexpr int  STA_RETRY_MS = 5000;   // délai avant nouvelle tentative de connexion STA

// Noms des commandes WebSocket (client → serveur) et réponses
constexpr char CMD_REBOOT[]    = "reboot";
constexpr char CMD_CALIBRATE[] = "calibrate";
constexpr char CMD_WIFISET[]   = "wifiset";
constexpr char CMD_WIFIGET[]   = "wifiget";
constexpr char CMD_HIST[]      = "hist";
constexpr char CMD_SET[]       = "\"set\"";
constexpr char CMD_STATUS[]    = "status";
constexpr char CMD_SYSINFO[]   = "sysinfo";
constexpr char REPLY_OK[]      = "{\"type\":\"ok\"}";

httpd_handle_t     m_server = nullptr;
esp_timer_handle_t m_sta_retry = nullptr;
std::atomic<bool>  m_sta_connected{false};
char               m_sta_ip[16] = "0.0.0.0";
esp_netif_t*       m_netif_ap = nullptr;
esp_netif_t*       m_netif_sta = nullptr;

// Reconnexion STA temporisée (évite de boucler trop vite).
void staRetryCb(void*)
{
    esp_wifi_connect();
}

void wifiEvent(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (WIFI_EVENT == base && WIFI_EVENT_STA_START == id)
    {
        esp_wifi_connect();
    }
    else if (WIFI_EVENT == base && WIFI_EVENT_STA_CONNECTED == id)
    {
        // Demande une adresse IPv6 link-local sur la station (déclenche IP_EVENT_GOT_IP6).
        if (m_netif_sta) esp_netif_create_ip6_linklocal(m_netif_sta);
    }
    else if (WIFI_EVENT == base && WIFI_EVENT_AP_START == id)
    {
        if (m_netif_ap) esp_netif_create_ip6_linklocal(m_netif_ap);
    }
    else if (WIFI_EVENT == base && WIFI_EVENT_STA_DISCONNECTED == id)
    {
        m_sta_connected = false;
        std::strcpy(m_sta_ip, "0.0.0.0");
        if (m_sta_retry)
        {
            esp_timer_start_once(m_sta_retry, static_cast<int64_t>(STA_RETRY_MS) * 1000);   // 5 s
        }
    }
    else if (IP_EVENT == base && IP_EVENT_STA_GOT_IP == id)
    {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        snprintf(m_sta_ip, sizeof(m_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        m_sta_connected = true;
        ESP_LOGI(TAG, "STA connecté, IP %s", m_sta_ip);
    }
    else if (IP_EVENT == base && IP_EVENT_GOT_IP6 == id)
    {
        auto* ev = static_cast<ip_event_got_ip6_t*>(data);
        char buf[48];
        snprintf(buf, sizeof(buf), IPV6STR, IPV62STR(ev->ip6_info.ip));
        ESP_LOGI(TAG, "IPv6 (%s) : %s",
                 (m_netif_ap && ev->esp_netif == m_netif_ap) ? "AP" : "STA", buf);
    }
}

// Extrait une chaîne JSON "clé":"valeur" (pas de gestion des échappements).
std::string jsonStr(const std::string& s, const char* key)
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = s.find(pattern);
    if (std::string::npos == p)
    {
        return "";
    }
    p = s.find(':', p + pattern.size());
    if (std::string::npos == p)
    {
        return "";
    }
    p = s.find('"', p);
    if (std::string::npos == p)
    {
        return "";
    }
    size_t q = s.find('"', p + 1);
    if (std::string::npos == q)
    {
        return "";
    }
    return s.substr(p + 1, q - p - 1);
}

std::string buildWifiJson()
{
    char ssid[33];
    bool enabled = false;
    configGetWifi(ssid, sizeof(ssid), nullptr, 0, &enabled);
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"wifi\",\"enabled\":%s,\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\"}",
             enabled ? "true" : "false", ssid, m_sta_connected.load() ? "true" : "false", m_sta_ip);
    return buf;
}

// ── Historique en RAM : rapide 10 min @ 1 s ; batterie 30 min @ 5 s ──
constexpr int HIST_FAST_N = 600;    // 10 min @ 1 s
constexpr int HIST_BATT_N = 360;    // 30 min @ 5 s
struct
{
    Ring<HIST_FAST_N> accel, pwml, pwmr, spd;
    Ring<HIST_BATT_N> batt;
    int tick = 0;
} m_hist;
SemaphoreHandle_t  m_hist_mtx = nullptr;
esp_timer_handle_t m_hist_timer = nullptr;

uint8_t pctU8(float v)
{
    if (v < 0.f) v = 0.f;
    if (v > 100.f) v = 100.f;
    return static_cast<uint8_t>(v + 0.5f);
}

void histSample(void*)
{
    KartConfig cfg = configSnapshot();
    float lim = cfg.speed_limit_kmh;
    if (lim < 1.f) lim = 1.f;

    xSemaphoreTake(m_hist_mtx, portMAX_DELAY);
    m_hist.accel.push(pctU8(g_status.m_throttle.load() * 100.f));
    m_hist.pwml.push(pctU8(fabsf(g_status.m_out_l.load()) * 100.f));
    m_hist.pwmr.push(pctU8(fabsf(g_status.m_out_r.load()) * 100.f));
    m_hist.spd.push(pctU8(g_status.m_speed.load() / lim * 100.f));
    if (0 == (m_hist.tick % 5))   // batterie toutes les 5 s
    {
        int cells = iround(cfg.cell_count);
        if (cells < 1) cells = 1;
        float low = cells * 3.0f;          // ~vide (3,0 V/cellule)
        float full = cells * 4.2f;         // pleine charge (4,2 V/cellule)
        m_hist.batt.push(pctU8((g_status.m_vbat.load() - low) / (full - low) * 100.f));
    }
    ++m_hist.tick;
    xSemaphoreGive(m_hist_mtx);
}

std::string buildHistJson()
{
    std::string out = "{\"type\":\"hist\",\"dt_fast\":1,\"dt_batt\":5,";
    xSemaphoreTake(m_hist_mtx, portMAX_DELAY);
    m_hist.accel.appendJson(out, "accel"); out += ',';
    m_hist.pwml.appendJson(out, "pwml");   out += ',';
    m_hist.pwmr.appendJson(out, "pwmr");   out += ',';
    m_hist.spd.appendJson(out, "spd");     out += ',';
    m_hist.batt.appendJson(out, "batt");
    xSemaphoreGive(m_hist_mtx);
    out += '}';
    return out;
}

std::string buildConfigJson()
{
    KartConfig cfg = configSnapshot();
    std::string out = "{\"type\":\"config\",\"params\":[";
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        const ParamDesc& p = PARAMS[i];
        const char* type = (PType::Float == p.type) ? "float" : (PType::Int == p.type ? "int" : "bool");
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%s{\"name\":\"%s\",\"desc\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"val\":%g}",
                 i ? "," : "", p.name, p.desc, type, p.min, p.max, cfg.*(PARAMS[i].field));
        out += buf;
    }
    out += "]}";
    return out;
}

std::string buildStatusJson()
{
    char buf[400];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"status\",\"state\":%d,\"fault\":%d,"
             "\"thr_raw\":%d,\"throttle\":%.3f,\"vbat\":%.2f,\"speed\":%.2f,"
             "\"btn_start\":%s,\"btn_rev\":%s,"
             "\"out_l\":%.3f,\"out_r\":%.3f,\"brake\":%s,\"rev_led\":%s}",
             g_status.m_state.load(), g_status.m_fault.load(),
             g_status.m_thr_raw.load(), g_status.m_throttle.load(), g_status.m_vbat.load(),
             g_status.m_speed.load(),
             g_status.m_btn_start.load() ? "true" : "false",
             g_status.m_btn_rev.load() ? "true" : "false",
             g_status.m_out_l.load(), g_status.m_out_r.load(),
             g_status.m_brake.load() ? "true" : "false",
             g_status.m_rev_led.load() ? "true" : "false");
    return buf;
}

std::string macStr(wifi_interface_t iface)
{
    uint8_t m[6] = {0};
    esp_wifi_get_mac(iface, m);
    char b[18];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

const char* resetReasonStr(esp_reset_reason_t r)
{
    switch (r)
    {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "logiciel";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP:return "deep-sleep";
        default:               return "autre";
    }
}

const char* ip6TypeStr(int t)
{
    switch (t)
    {
        case ESP_IP6_ADDR_IS_LINK_LOCAL:        return "link-local";
        case ESP_IP6_ADDR_IS_GLOBAL:            return "global";
        case ESP_IP6_ADDR_IS_UNIQUE_LOCAL:      return "unique-local";
        case ESP_IP6_ADDR_IS_SITE_LOCAL:        return "site-local";
        case ESP_IP6_ADDR_IS_IPV4_MAPPED_IPV6:  return "ipv4-mapped";
        default:                                return "autre";
    }
}

// Toutes les adresses IPv6 d'une interface (link-local + SLAAC…) en tableau JSON.
std::string ip6ArrayJson(esp_netif_t* netif)
{
    if (!netif) return "[]";
    esp_ip6_addr_t a[5];
    int n = esp_netif_get_all_ip6(netif, a);
    std::string out = "[";
    for (int i = 0; i < n; ++i)
    {
        char b[96];
        snprintf(b, sizeof(b), "%s{\"addr\":\"" IPV6STR "\",\"type\":\"%s\"}",
                 i ? "," : "", IPV62STR(a[i]), ip6TypeStr(esp_netif_ip6_get_addr_type(&a[i])));
        out += b;
    }
    out += "]";
    return out;
}

// Infos système (page « Système ») : version/commit, uptime, MAC, IP v4/v6, heap, chip…
std::string buildSysInfoJson()
{
    const esp_app_desc_t* app = esp_app_get_description();

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    uint32_t flash_sz = 0;
    esp_flash_get_size(nullptr, &flash_sz);

    int64_t up_s = esp_timer_get_time() / 1000000;

    esp_netif_ip_info_t ap_ip{};
    if (m_netif_ap) esp_netif_get_ip_info(m_netif_ap, &ap_ip);

    char ssid[33] = "";
    bool sta_en = false;
    configGetWifi(ssid, sizeof(ssid), nullptr, 0, &sta_en);

    char buf[760];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"sysinfo\","
        "\"fw_ver\":\"%s\",\"fw_proj\":\"%s\",\"fw_date\":\"%s %s\",\"idf\":\"%s\","
        "\"chip\":\"ESP32\",\"cores\":%d,\"rev\":%d,\"flash_mb\":%lu,"
        "\"uptime_s\":%lld,\"heap_free\":%lu,\"heap_min\":%lu,\"reset\":\"%s\","
        "\"mac_ap\":\"%s\",\"mac_sta\":\"%s\","
        "\"ap_ssid\":\"%s\",\"ip_ap\":\"" IPSTR "\","
        "\"sta_en\":%s,\"sta_ssid\":\"%s\",\"sta_conn\":%s,\"ip_sta\":\"%s\"",
        app ? app->version : "?", app ? app->project_name : "?",
        app ? app->date : "?", app ? app->time : "", app ? app->idf_ver : "?",
        chip.cores, chip.revision, static_cast<unsigned long>(flash_sz / (1024 * 1024)),
        static_cast<long long>(up_s),
        static_cast<unsigned long>(esp_get_free_heap_size()),
        static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
        resetReasonStr(esp_reset_reason()),
        macStr(WIFI_IF_AP).c_str(), macStr(WIFI_IF_STA).c_str(),
        AP_SSID, IP2STR(&ap_ip.ip),
        sta_en ? "true" : "false", ssid,
        m_sta_connected.load() ? "true" : "false", m_sta_ip);
    // Toutes les adresses IPv6 de chaque interface (link-local + SLAAC…)
    std::string out = buf;
    out += ",\"ip6_ap\":"  + ip6ArrayJson(m_netif_ap);
    out += ",\"ip6_sta\":" + ip6ArrayJson(m_netif_sta);
    out += "}";
    return out;
}

// Mini-parseur JSON plat (suffisant pour notre client contrôlé)
bool jsonNum(const std::string& s, const char* key, double& out)
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = s.find(pattern);
    if (std::string::npos == p)
    {
        return false;
    }
    p = s.find(':', p + pattern.size());
    if (std::string::npos == p)
    {
        return false;
    }
    ++p;
    while (p < s.size() && (' ' == s[p] || '\t' == s[p]))
    {
        ++p;
    }
    if (0 == s.compare(p, 4, "true"))
    {
        out = 1;
        return true;
    }
    if (0 == s.compare(p, 5, "false"))
    {
        out = 0;
        return true;
    }
    char* end = nullptr;
    double v = strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p)
    {
        return false;
    }
    out = v;
    return true;
}

std::string readBody(httpd_req_t* req)
{
    std::string body;
    if (req->content_len <= 0 || req->content_len > 4096)
    {
        return body;
    }
    body.resize(req->content_len);
    int off = 0;
    while (off < req->content_len)
    {
        int r = httpd_req_recv(req, &body[off], req->content_len - off);
        if (r <= 0)
        {
            break;
        }
        off += r;
    }
    body.resize(off);
    return body;
}

void applyConfigJson(const std::string& body)
{
    KartConfig cfg = configSnapshot();
    double v;
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        if (jsonNum(body, PARAMS[i].name, v))
        {
            cfg.*(PARAMS[i].field) = static_cast<float>(v);
        }
    }
    configUpdate(cfg, true);
}

esp_err_t rootGet(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

esp_err_t styleGet(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, style_css_start, HTTPD_RESP_USE_STRLEN);
}

esp_err_t wsReply(httpd_req_t* req, const std::string& s)
{
    httpd_ws_frame_t frame{};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(s.data()));
    frame.len = s.size();
    return httpd_ws_send_frame(req, &frame);
}

esp_err_t wsHandler(httpd_req_t* req)
{
    if (HTTP_GET == req->method)
    {
        return ESP_OK;   // handshake : le client demandera
    }

    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_TEXT;
    if (ESP_OK != httpd_ws_recv_frame(req, &frame, 0))
    {
        return ESP_FAIL;
    }
    if (0 == frame.len || frame.len > 2048)
    {
        return ESP_OK;
    }

    std::string body(frame.len, '\0');
    frame.payload = reinterpret_cast<uint8_t*>(body.data());
    if (ESP_OK != httpd_ws_recv_frame(req, &frame, frame.len))
    {
        return ESP_FAIL;
    }

    if (std::string::npos != body.find(CMD_REBOOT))
    {
        wsReply(req, REPLY_OK);
        ESP_LOGW(TAG, "Redémarrage demandé via le web");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();   // ne revient pas ; au boot → désarmé
        return ESP_OK;
    }
    if (std::string::npos != body.find(CMD_CALIBRATE))
    {
        g_status.m_cmd.store(1);
        return wsReply(req, REPLY_OK);
    }
    if (std::string::npos != body.find(CMD_WIFISET))
    {
        double v;
        bool enabled = jsonNum(body, "enabled", v) ? (0.0 != v) : true;
        configSetWifi(jsonStr(body, "ssid").c_str(), jsonStr(body, "pass").c_str(), enabled);
        return wsReply(req, REPLY_OK);   // prise en compte au redémarrage
    }
    if (std::string::npos != body.find(CMD_WIFIGET))
    {
        return wsReply(req, buildWifiJson());
    }
    if (std::string::npos != body.find(CMD_HIST))
    {
        return wsReply(req, buildHistJson());
    }
    if (std::string::npos != body.find(CMD_SET))
    {
        applyConfigJson(body);
        return wsReply(req, buildConfigJson());
    }
    if (std::string::npos != body.find(CMD_SYSINFO))
    {
        return wsReply(req, buildSysInfoJson());
    }
    if (std::string::npos != body.find(CMD_STATUS))
    {
        return wsReply(req, buildStatusJson());
    }
    return wsReply(req, buildConfigJson());   // "get" par défaut
}
} // namespace

void wifiSoftAPInit()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    m_netif_ap  = esp_netif_create_default_wifi_ap();
    m_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr, nullptr));

    // Point d'accès (toujours actif)
    wifi_config_t ap{};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = std::strlen(AP_SSID);
    std::strncpy(reinterpret_cast<char*>(ap.ap.password), AP_PASS, sizeof(ap.ap.password));
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode = (std::strlen(AP_PASS) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    // Station (si des identifiants sont enregistrés) → mode AP+STA
    char ssid[33];
    char pass[65];
    bool have_sta = configGetWifi(ssid, sizeof(ssid), pass, sizeof(pass));

    ESP_ERROR_CHECK(esp_wifi_set_mode(have_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    if (have_sta)
    {
        wifi_config_t sta{};
        std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), ssid, sizeof(sta.sta.ssid));
        std::strncpy(reinterpret_cast<char*>(sta.sta.password), pass, sizeof(sta.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

        esp_timer_create_args_t targs{};
        targs.callback = staRetryCb;
        targs.name = "sta_retry";
        ESP_ERROR_CHECK(esp_timer_create(&targs, &m_sta_retry));
        ESP_LOGI(TAG, "Mode AP+STA : connexion à « %s »", ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP « %s » prêt → http://192.168.4.1", AP_SSID);
}

void webServerStart()
{
    // Historique en RAM (échantillonné toutes les secondes)
    m_hist_mtx = xSemaphoreCreateMutex();
    esp_timer_create_args_t ht{};
    ht.callback = histSample;
    ht.name = "hist";
    ESP_ERROR_CHECK(esp_timer_create(&ht, &m_hist_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(m_hist_timer, 1000000));   // 1 s

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (ESP_OK != httpd_start(&m_server, &config))
    {
        ESP_LOGE(TAG, "Échec démarrage HTTP");
        return;
    }
    auto reg = [](const char* uri, esp_err_t (*handler)(httpd_req_t*), bool is_ws)
    {
        httpd_uri_t u{};
        u.uri = uri;
        u.method = HTTP_GET;
        u.handler = handler;
        u.user_ctx = nullptr;
        u.is_websocket = is_ws;
        httpd_register_uri_handler(m_server, &u);
    };
    reg("/",          rootGet,  false);
    reg("/style.css", styleGet, false);
    reg("/ws",        wsHandler, true);
    ESP_LOGI(TAG, "Serveur web + WebSocket démarrés");
}
