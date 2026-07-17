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

#include "config.hpp"
#include "hardware.hpp"
#include "input.hpp"
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
// Assets compressés (gzip) embarqués — servis tels quels avec Content-Encoding: gzip.
extern const uint8_t index_html_gz_start[]   asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]     asm("_binary_index_html_gz_end");
extern const uint8_t style_css_gz_start[]    asm("_binary_style_css_gz_start");
extern const uint8_t style_css_gz_end[]      asm("_binary_style_css_gz_end");
extern const uint8_t chart_min_js_gz_start[] asm("_binary_chart_min_js_gz_start");
extern const uint8_t chart_min_js_gz_end[]   asm("_binary_chart_min_js_gz_end");

namespace
{
constexpr char AP_SSID[]  = "Kart-Config";
constexpr char AP_PASS[]  = "kart12345";   // ≥ 8 caractères (sinon AP ouvert)
constexpr int  AP_CHANNEL = 1;
constexpr int  AP_MAX_CONN = 4;
constexpr int  STA_RETRY_MS = 5000;   // délai avant nouvelle tentative de connexion STA

// Noms des commandes WebSocket (client → serveur) et réponses
constexpr char CMD_SET[]       = "\"set\"";
constexpr char CMD_VALS[]      = "\"vals\"";  // valeurs des paramètres (métadonnées : « get », 1× à l'ouverture)
// Manette Bluetooth
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

// ── Réponses WebSocket SANS TAS ──
// La tâche httpd sert les requêtes UNE PAR UNE : un tampon statique unique suffit pour
// toutes les réponses (aucune réentrance possible). Prévisibilité : ZÉRO allocation
// dynamique dans le chemin WebSocket, quels que soient l'uptime et la charge — leçon du
// crash « tas épuisé » (voir hist) : les pics d'allocation sous 2 clients gelaient la radio.
constexpr size_t REPLY_CAP = 8192;   // plus grosse réponse : config (~6,2 ko) + marge
char   m_reply[REPLY_CAP];
size_t m_rlen = 0;

void rReset() { m_rlen = 0; m_reply[0] = '\0'; }

void rPut(const char* str)
{
    while (*str && m_rlen + 1 < REPLY_CAP) m_reply[m_rlen++] = *str++;
    m_reply[m_rlen] = '\0';
}

void rFmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
void rFmt(const char* f, ...)
{
    va_list ap;
    va_start(ap, f);
    const int n = vsnprintf(m_reply + m_rlen, REPLY_CAP - m_rlen, f, ap);
    va_end(ap);
    if (n > 0) m_rlen = std::min(m_rlen + static_cast<size_t>(n), REPLY_CAP - 1);
}

// Échappement JSON borné (guillemets, antislash, contrôles) — pour les textes non
// maîtrisés : nom de la manette, SSID saisi par l'utilisateur.
void rEsc(const char* in)
{
    for (const char* p = in; p && *p; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if ('"' == c || '\\' == c)      rFmt("\\%c", c);
        else if (c < 0x20)              rFmt("\\u%04x", c);
        else                            rFmt("%c", c);
    }
}

// Extrait une chaîne JSON "clé":"valeur" dans out (borné, sans gestion des échappements).
bool jsonStrC(const char* s, const char* key, char* out, size_t cap)
{
    out[0] = '\0';
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    ++p;
    const char* q = strchr(p, '"');
    if (!q) return false;
    size_t n = static_cast<size_t>(q - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

void buildWifiJson()
{
    char ssid[33];
    bool enabled = false;
    configGetWifi(ssid, sizeof(ssid), nullptr, 0, &enabled);
    rReset();
    rFmt("{\"type\":\"wifi\",\"enabled\":%s,\"ssid\":\"", enabled ? "true" : "false");
    rEsc(ssid);
    rFmt("\",\"connected\":%s,\"ip\":\"%s\"}",
         m_sta_connected.load() ? "true" : "false", m_sta_ip);
}

// ── Historique en RAM : mêmes fenêtres (10 min / 1 min / 30 min) mais échantillonnage
// moins dense — l'ancien 600+360 points donnait un JSON de ~17 ko à anneaux PLEINS ; avec
// 2 clients simultanés, les pics d'allocation épuisaient le tas (~40 ko libres, heap_min
// mesuré < 11 ko) → malloc en échec dans les internals radio → gels/watchdog. ~120 points
// suffisent largement à l'écran (graphique ~520 px).
constexpr int HIST_FAST_N   = 120;  // 10 min @ 5 s
constexpr int HIST_FAST_DT  = 5;
constexpr int HIST_SPEED_N  = 60;   // 1 min @ 1 s (graphique vitesse dédié, petit)
constexpr int HIST_BATT_N   = 120;  // 30 min @ 15 s
constexpr int HIST_BATT_DT  = 15;
struct
{
    Ring<HIST_FAST_N>  accel, pwml, pwmr, rpml, rpmr;   // rpml/rpmr : tr/min roue (0..250)
    Ring<HIST_SPEED_N> spd;
    Ring<HIST_BATT_N>  batt;
    int tick = 0;
} m_hist;
SemaphoreHandle_t  m_hist_mtx = nullptr;
esp_timer_handle_t m_hist_timer = nullptr;

uint8_t pctU8(float v)   // pourcentage 0..100 (accélérateur, PWM)
{
    if (v < 0.f) v = 0.f;
    if (v > 100.f) v = 100.f;
    return static_cast<uint8_t>(v + 0.5f);
}

uint8_t u8x10(float v)   // valeur physique ×10 (résolution 0,1 ; bornée 0..25,5 → km/h, V)
{
    float s = v * 10.f;
    if (s < 0.f) s = 0.f;
    if (s > 255.f) s = 255.f;
    return static_cast<uint8_t>(s + 0.5f);
}

// Vitesse roue (m/s) → tr/min, bornée 0..250 (octet ; ~3,3 m/s max avant saturation en roue 10").
uint8_t rpmU8(float ms)
{
    const float circ_m = 3.14159265f * hw::WHEEL_DIAM_M;   // circonférence roue (m)
    float rpm = fabsf(ms) * 60.f / circ_m;
    if (rpm > 250.f) rpm = 250.f;
    return static_cast<uint8_t>(rpm + 0.5f);
}

void histSample(void*)
{
    // Tourne dans la tâche esp_timer (priorité 22, partagée avec les timers Wi-Fi/BT) :
    // il est INTERDIT d'y bloquer. Mutex occupé (un client construit le JSON) → on saute
    // l'échantillon, tant pis — l'historique est indicatif.
    if (pdTRUE != xSemaphoreTake(m_hist_mtx, 0)) return;
    m_hist.spd.push(u8x10(fabsf(g_status.m_speed_ms.load())));               // |v véhicule| m/s ×10, chaque s
    if (0 == (m_hist.tick % HIST_FAST_DT))
    {
        m_hist.accel.push(pctU8(fabsf(g_status.m_fwd.load()) * 100.f));      // % (consigne avance)
        m_hist.pwml.push(pctU8(fabsf(g_status.m_out_l.load()) * 100.f));     // %
        m_hist.pwmr.push(pctU8(fabsf(g_status.m_out_r.load()) * 100.f));     // %
        m_hist.rpml.push(rpmU8(g_status.m_speed_l.load()));                  // tr/min roue G
        m_hist.rpmr.push(rpmU8(g_status.m_speed_r.load()));                  // tr/min roue D
    }
    if (0 == (m_hist.tick % HIST_BATT_DT))
    {
        m_hist.batt.push(u8x10(g_status.m_vbat.load()));                     // V ×10
    }
    ++m_hist.tick;
    xSemaphoreGive(m_hist_mtx);
}

// Cache statique du JSON d'historique : reconstruit AU PLUS une fois par seconde (tous les
// clients reçoivent la même chaîne), dans un tampon FIXE — plein = ~1,6 ko, capacité 3 ko.
// Seule la tâche httpd écrit/li(t) ce cache ; le mutex ne protège que les anneaux.
char    m_hist_json[3072];
size_t  m_hist_json_len = 0;
int64_t m_hist_json_us = -1;

const char* buildHistJson(size_t& len)
{
    const int64_t now = esp_timer_get_time();
    if (m_hist_json_us >= 0 && (now - m_hist_json_us) < 1000000)
    {
        len = m_hist_json_len;
        return m_hist_json;
    }
    size_t n = static_cast<size_t>(
        snprintf(m_hist_json, sizeof(m_hist_json),
                 "{\"type\":\"hist\",\"dt_fast\":5,\"dt_spd\":1,\"dt_batt\":15,"));
    xSemaphoreTake(m_hist_mtx, portMAX_DELAY);
    struct { const Ring<HIST_FAST_N>* r; const char* k; } fast[] = {
        {&m_hist.accel, "accel"}, {&m_hist.pwml, "pwml"}, {&m_hist.pwmr, "pwmr"},
        {&m_hist.rpml, "rpml"},   {&m_hist.rpmr, "rpmr"},
    };
    for (auto& f : fast)
    {
        n += f.r->appendJsonC(m_hist_json + n, sizeof(m_hist_json) - n, f.k);
        if (n + 1 < sizeof(m_hist_json)) m_hist_json[n++] = ',';
    }
    n += m_hist.spd.appendJsonC(m_hist_json + n, sizeof(m_hist_json) - n, "spd");
    if (n + 1 < sizeof(m_hist_json)) m_hist_json[n++] = ',';
    n += m_hist.batt.appendJsonC(m_hist_json + n, sizeof(m_hist_json) - n, "batt");
    xSemaphoreGive(m_hist_mtx);
    if (n + 1 < sizeof(m_hist_json)) m_hist_json[n++] = '}';
    m_hist_json[n] = '\0';
    m_hist_json_len = n;
    m_hist_json_us = now;
    len = n;
    return m_hist_json;
}

void buildConfigJson()
{
    const KartConfig cfg = configSnapshot();
    rReset();
    rPut("{\"type\":\"config\",\"params\":[");
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        const ParamDesc& p = PARAMS[i];
        const char* type = (PType::Float == p.type) ? "float" : (PType::Int == p.type ? "int" : "bool");
        if (i) rPut(",");
        // desc/cat/help : textes statiques SANS guillemets doubles (contrat de config.cpp)
        rFmt("{\"name\":\"%s\",\"desc\":\"%s\",\"cat\":\"%s\",\"help\":\"%s\","
             "\"type\":\"%s\",\"min\":%g,\"max\":%g,\"val\":%g}",
             p.name, p.desc, p.cat, p.help, type, p.min, p.max, cfg.*(PARAMS[i].field));
    }
    rPut("]}");
}

// Échelle d'affichage de la jauge batterie — décidée ICI (le client ne connaît aucun seuil) :
// bas = seuil de coupure LVC du type détecté, haut = tension pleine charge au repos.
float battDispLo()
{
    const int bt = g_status.m_batt_type.load();
    return (24 == bt) ? hw::VBAT24_CUT_V : (12 == bt) ? hw::VBAT12_CUT_V : 0.f;
}
float battDispHi()
{
    const int bt = g_status.m_batt_type.load();
    return (24 == bt) ? hw::VBAT24_FULL_V : (12 == bt) ? hw::VBAT12_FULL_V : 0.f;
}

// Valeurs des paramètres SEULES (~0,6 ko) : les métadonnées (desc/cat/help/min/max) sont
// immuables en cours d'exécution et ne partent qu'avec « get », une fois à l'ouverture.
void buildValsJson()
{
    const KartConfig cfg = configSnapshot();
    rReset();
    rPut("{\"type\":\"vals\",\"params\":[");
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        rFmt("%s{\"name\":\"%s\",\"val\":%g}", i ? "," : "", PARAMS[i].name, cfg.*(PARAMS[i].field));
    }
    rPut("]}");
}

void buildStatusJson()
{
    rReset();
    rFmt(
             "{\"type\":\"status\",\"state\":%d,\"fault\":%d,\"faults\":%u,\"vbat\":%.2f,"
             "\"batt_type\":%d,\"batt_lo\":%.1f,\"batt_hi\":%.1f,"
             "\"speed_ms\":%.2f,\"speed_l\":%.2f,\"speed_r\":%.2f,\"fwd\":%.3f,\"turn\":%.3f,"
             "\"out_l\":%.3f,\"out_r\":%.3f,\"brake_mode\":%d,\"arming\":%s,\"btn_start\":%s,"
             "\"pad_conn\":%s,\"pad_batt\":%d,\"pad_x\":%.3f,\"pad_y\":%.3f,"
             "\"pad_cx\":%.3f,\"pad_cy\":%.3f,\"pad_zl\":%.2f,\"pad_zr\":%.2f,"
             "\"pad_rx2\":%.3f,\"pad_ry2\":%.3f,\"pad_btns\":%u,\"pad_age_ms\":%d}",
             g_status.m_state.load(), g_status.m_fault.load(), g_status.m_faults.load(),
             g_status.m_vbat.load(), g_status.m_batt_type.load(),
             battDispLo(), battDispHi(),
             g_status.m_speed_ms.load(),
             g_status.m_speed_l.load(), g_status.m_speed_r.load(),
             g_status.m_fwd.load(), g_status.m_turn.load(),
             g_status.m_out_l.load(), g_status.m_out_r.load(),
             g_status.m_brake_mode.load(),
             g_status.m_arming.load() ? "true" : "false",
             g_status.m_btn_start.load() ? "true" : "false",
             g_status.m_pad_conn.load() ? "true" : "false",
             g_status.m_pad_batt.load(),
             g_status.m_pad_x.load(), g_status.m_pad_y.load(),
             g_status.m_pad_cx.load(), g_status.m_pad_cy.load(),
             g_status.m_pad_zl.load(), g_status.m_pad_zr.load(),
             g_status.m_pad_rx2.load(), g_status.m_pad_ry2.load(),
             g_status.m_pad_btns.load(),
             static_cast<int>(std::min<int64_t>((esp_timer_get_time() - input::lastReportUs()) / 1000, 99999)));
}

// Infos manette (onglet Manette) : connexion, modèle, batterie, calibration, appairage.
void buildPadJson()
{
    rReset();
    rFmt("{\"type\":\"pad\",\"conn\":%s,\"name\":\"",
         g_status.m_pad_conn.load() ? "true" : "false");
    rEsc(input::name());
    rFmt("\",\"batt\":%d,\"calibrated\":%s,\"calstate\":%d,\"pairing\":%s}",
         input::battery(),
         input::calibrated() ? "true" : "false",
         input::calState(),
         input::pairing() ? "true" : "false");
}

void macTo(char (&b)[18], wifi_interface_t iface)
{
    uint8_t m[6] = {0};
    esp_wifi_get_mac(iface, m);
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
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

// Toutes les adresses IPv6 d'une interface (link-local + SLAAC…) en tableau JSON → m_reply.
void ip6ArrayAppend(esp_netif_t* netif)
{
    rPut("[");
    if (netif)
    {
        esp_ip6_addr_t a[5];
        const int n = esp_netif_get_all_ip6(netif, a);
        for (int i = 0; i < n; ++i)
        {
            rFmt("%s{\"addr\":\"" IPV6STR "\",\"type\":\"%s\"}",
                 i ? "," : "", IPV62STR(a[i]), ip6TypeStr(esp_netif_ip6_get_addr_type(&a[i])));
        }
    }
    rPut("]");
}

// Infos système (page « Système ») : version/commit, uptime, MAC, IP v4/v6, heap, chip…
// Partie DYNAMIQUE de l'onglet Système : le reste (puce, MAC, version…) est immuable et
// ne part qu'avec « sysinfo », une fois — le navigateur garde la table construite.
void buildSysDynJson()
{
    rReset();
    rFmt("{\"type\":\"sysdyn\",\"uptime_s\":%lld,\"heap_free\":%lu,\"heap_min\":%lu,\"ledc_fix\":%lu}",
         static_cast<long long>(esp_timer_get_time() / 1000000),
         static_cast<unsigned long>(esp_get_free_heap_size()),
         static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
         static_cast<unsigned long>(board::ledcClkFixCount()));
}

void buildSysInfoJson()
{
    const esp_app_desc_t* app = esp_app_get_description();

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    uint32_t flash_sz = 0;
    esp_flash_get_size(nullptr, &flash_sz);

    const int64_t up_s = esp_timer_get_time() / 1000000;

    esp_netif_ip_info_t ap_ip{};
    if (m_netif_ap) esp_netif_get_ip_info(m_netif_ap, &ap_ip);

    char ssid[33] = "";
    bool sta_en = false;
    configGetWifi(ssid, sizeof(ssid), nullptr, 0, &sta_en);

    char mac_ap[18], mac_sta[18];
    macTo(mac_ap, WIFI_IF_AP);
    macTo(mac_sta, WIFI_IF_STA);
    rReset();
    rFmt(
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
        mac_ap, mac_sta,
        AP_SSID, IP2STR(&ap_ip.ip),
        sta_en ? "true" : "false", ssid,
        m_sta_connected.load() ? "true" : "false", m_sta_ip);
    // Toutes les adresses IPv6 de chaque interface (link-local + SLAAC…)
    rPut(",\"ip6_ap\":");
    ip6ArrayAppend(m_netif_ap);
    rPut(",\"ip6_sta\":");
    ip6ArrayAppend(m_netif_sta);
    rPut("}");
}

// Mini-parseur JSON plat SANS TAS (suffisant pour notre client contrôlé)
bool jsonNumC(const char* s, const char* key, double& out)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    ++p;
    while (' ' == *p || '\t' == *p) ++p;
    if (0 == strncmp(p, "true", 4))  { out = 1; return true; }
    if (0 == strncmp(p, "false", 5)) { out = 0; return true; }
    char* end = nullptr;
    const double v = strtod(p, &end);
    if (end == p) return false;
    out = v;
    return true;
}

void applyConfigJson(const char* body)
{
    KartConfig cfg = configSnapshot();
    double v;
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        if (jsonNumC(body, PARAMS[i].name, v))
        {
            cfg.*(PARAMS[i].field) = static_cast<float>(v);
        }
    }
    configUpdate(cfg, true);
}

esp_err_t sendGz(httpd_req_t* req, const char* ctype, const uint8_t* s, const uint8_t* e)
{
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, reinterpret_cast<const char*>(s), e - s);
}

esp_err_t rootGet(httpd_req_t* req)
{
    return sendGz(req, "text/html", index_html_gz_start, index_html_gz_end);
}

esp_err_t styleGet(httpd_req_t* req)
{
    return sendGz(req, "text/css", style_css_gz_start, style_css_gz_end);
}

esp_err_t chartGet(httpd_req_t* req)
{
    return sendGz(req, "text/javascript", chart_min_js_gz_start, chart_min_js_gz_end);
}

esp_err_t wsSend(httpd_req_t* req, const char* payload, size_t len)
{
    httpd_ws_frame_t frame{};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    frame.len = len;
    return httpd_ws_send_frame(req, &frame);
}

esp_err_t wsReply(httpd_req_t* req)   // envoie l'arène de réponse (m_reply)
{
    return wsSend(req, m_reply, m_rlen);
}

// Tampon de RÉCEPTION statique (même logique : la tâche httpd est séquentielle).
constexpr size_t BODY_CAP = 2048;
char m_body[BODY_CAP + 1];

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
    if (0 == frame.len || frame.len > BODY_CAP)
    {
        return ESP_OK;
    }

    frame.payload = reinterpret_cast<uint8_t*>(m_body);
    if (ESP_OK != httpd_ws_recv_frame(req, &frame, frame.len))
    {
        return ESP_FAIL;
    }
    m_body[frame.len] = '\0';

    // Dispatch EXACT sur le champ "type" (strcmp) : contrairement à une recherche de
    // sous-chaîne dans tout le corps, aucune valeur de paramètre ni nom futur ne peut
    // détourner une commande, et l'ordre des comparaisons est sans importance.
    char cmd[16];
    jsonStrC(m_body, "type", cmd, sizeof(cmd));

    if (0 == strcmp(cmd, "reboot"))
    {
        rReset(); rPut(REPLY_OK); wsReply(req);
        ESP_LOGW(TAG, "Redémarrage demandé via le web");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();   // ne revient pas ; au boot → désarmé
        return ESP_OK;
    }
    if (0 == strcmp(cmd, "padunpair")) { input::unpair();       buildPadJson(); return wsReply(req); }
    if (0 == strcmp(cmd, "padpair"))   { input::startPairing(); buildPadJson(); return wsReply(req); }   // ⚠️ efface la calibration
    if (0 == strcmp(cmd, "calstart"))  { input::calStart();     buildPadJson(); return wsReply(req); }
    if (0 == strcmp(cmd, "calfinish")) { input::calFinish();    buildPadJson(); return wsReply(req); }
    if (0 == strcmp(cmd, "calcancel")) { input::calCancel();    buildPadJson(); return wsReply(req); }
    if (0 == strcmp(cmd, "padinfo"))   {                        buildPadJson(); return wsReply(req); }
    if (0 == strcmp(cmd, "wifiset"))
    {
        double v;
        const bool enabled = jsonNumC(m_body, "enabled", v) ? (0.0 != v) : true;
        char ssid[33], pass[65];
        jsonStrC(m_body, "ssid", ssid, sizeof(ssid));
        jsonStrC(m_body, "pass", pass, sizeof(pass));
        configSetWifi(ssid, pass, enabled);
        rReset(); rPut(REPLY_OK);
        return wsReply(req);   // prise en compte au redémarrage
    }
    if (0 == strcmp(cmd, "wifiget"))   { buildWifiJson();    return wsReply(req); }
    if (0 == strcmp(cmd, "vals"))      { buildValsJson();    return wsReply(req); }
    if (0 == strcmp(cmd, "sysdyn"))    { buildSysDynJson();  return wsReply(req); }
    if (0 == strcmp(cmd, "hist"))
    {
        size_t n = 0;
        const char* h = buildHistJson(n);
        return wsSend(req, h, n);
    }
    if (0 == strcmp(cmd, "set"))
    {
        applyConfigJson(m_body);
        buildValsJson();   // métadonnées déjà chez le client (« get » à l'ouverture)
        return wsReply(req);
    }
    if (0 == strcmp(cmd, "sysinfo"))   { buildSysInfoJson(); return wsReply(req); }
    if (0 == strcmp(cmd, "status"))    { buildStatusJson();  return wsReply(req); }
    buildConfigJson();   // "get" (et défaut)
    return wsReply(req);
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
    config.stack_size = 8192;   // défaut 4096 : trop juste pour WS + formatage %g (débordement vécu)
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
    reg("/chart.js",  chartGet, false);
    reg("/ws",        wsHandler, true);
    ESP_LOGI(TAG, "Serveur web + WebSocket démarrés");
}
