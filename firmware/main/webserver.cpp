// webserver.cpp — SoftAP + HTTP/WebSocket server to tune the config live.
//
// Connection: Wi-Fi "Kart-Config" (password "kart12345"), then
// open http://192.168.4.1 in a browser.
//
// The page (HTML/CSS) comes from main/assets/ (EMBED_TXTFILES). Comm over WebSocket /ws:
//   the client POLLS at 4 Hz and the server replies synchronously:
//     {"type":"status"}     → {"type":"status",...}
//     {"type":"get"}        → {"type":"config",...}
//     {"type":"set",...}    → applies + {"type":"config",...}
//     {"type":"calibrate"}  → triggers calibration
//     {"type":"reboot"}     → reboots the ESP32 (disarmed)
// In-house JSON (flat key/value) — no external dependency.
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
#include "pb_decode.h"
#include "pb_encode.h"
#include "proto/kart.pb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "web";

// Embedded files (EMBED_TXTFILES in main/CMakeLists.txt) — null-terminated.
// Embedded compressed (gzip) assets — served as-is with Content-Encoding: gzip.
extern const uint8_t index_html_gz_start[]   asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]     asm("_binary_index_html_gz_end");
extern const uint8_t style_css_gz_start[]    asm("_binary_style_css_gz_start");
extern const uint8_t style_css_gz_end[]      asm("_binary_style_css_gz_end");
extern const uint8_t chart_min_js_gz_start[] asm("_binary_chart_min_js_gz_start");
extern const uint8_t chart_min_js_gz_end[]   asm("_binary_chart_min_js_gz_end");
extern const uint8_t pb_min_js_gz_start[]    asm("_binary_pb_min_js_gz_start");
extern const uint8_t pb_min_js_gz_end[]      asm("_binary_pb_min_js_gz_end");

namespace
{
constexpr char AP_SSID[]  = "Kart-Config";
constexpr char AP_PASS[]  = "kart12345";   // ≥ 8 characters (otherwise open AP)
constexpr int  AP_CHANNEL = 1;
constexpr int  AP_MAX_CONN = 4;
constexpr int  STA_RETRY_MS = 5000;   // delay before a new STA connection attempt

// Names of the WebSocket commands (client → server) and responses
constexpr char CMD_SET[]       = "\"set\"";
constexpr char CMD_VALS[]      = "\"vals\"";  // parameter values (metadata: "get", once on open)
// Bluetooth gamepad

httpd_handle_t     m_server = nullptr;
esp_timer_handle_t m_sta_retry = nullptr;
std::atomic<bool>  m_sta_connected{false};
char               m_sta_ip[16] = "0.0.0.0";
esp_netif_t*       m_netif_ap = nullptr;
esp_netif_t*       m_netif_sta = nullptr;

// Timed STA reconnection (avoids looping too fast).
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
        // Request a link-local IPv6 address on the station (triggers IP_EVENT_GOT_IP6).
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
        ESP_LOGI(TAG, "STA connected, IP %s", m_sta_ip);
    }
    else if (IP_EVENT == base && IP_EVENT_GOT_IP6 == id)
    {
        auto* ev = static_cast<ip_event_got_ip6_t*>(data);
        char buf[48];
        snprintf(buf, sizeof(buf), IPV6STR, IPV62STR(ev->ip6_info.ip));
        ESP_LOGI(TAG, "IPv6 (%s): %s",
                 (m_netif_ap && ev->esp_netif == m_netif_ap) ? "AP" : "STA", buf);
    }
}

// ── WebSocket responses: PROTOBUF (nanopb), NO HEAP ──
// A single static encoding buffer (the httpd task is sequential: no reentrancy).
// nanopb encodes BY STREAM with callbacks: the strings (PARAMS, gamepad name…) and the
// history series go directly from their original pointers — zero copy,
// zero allocation, frames ~3× smaller than JSON.
constexpr size_t REPLY_CAP = 6144;   // largest response: config (4.7 KB binary, measured) + ~30 % margin
pb_byte_t m_reply[REPLY_CAP];

// Generic callback: encodes a C string (arg = const char*).
bool encStr(pb_ostream_t* os, const pb_field_t* field, void* const* arg)
{
    const char* str = static_cast<const char*>(*arg);
    if (!str) str = "";
    if (!pb_encode_tag_for_field(os, field)) return false;
    return pb_encode_string(os, reinterpret_cast<const pb_byte_t*>(str), strlen(str));
}

// Generic callback: encodes a block of bytes (arg = BytesArg*).
struct BytesArg
{
    const uint8_t* data;
    size_t         len;
};
bool encBytes(pb_ostream_t* os, const pb_field_t* field, void* const* arg)
{
    const BytesArg* b = static_cast<const BytesArg*>(*arg);
    if (!pb_encode_tag_for_field(os, field)) return false;
    return pb_encode_string(os, b->data, b->len);
}

// Encodes the Msg envelope into m_reply; returns the length (0 = failure, logged).
size_t encodeMsg(const Msg& msg)
{
    pb_ostream_t os = pb_ostream_from_buffer(m_reply, REPLY_CAP);
    if (!pb_encode(&os, Msg_fields, &msg))
    {
        ESP_LOGE(TAG, "Protobuf encoding: %s", PB_GET_ERROR(&os));
        return 0;
    }
    if (os.bytes_written > (REPLY_CAP * 4) / 5)
    {
        // Margin < 20 % (the config grows by ~190 B per added parameter): enlarge REPLY_CAP
        // BEFORE encoding silently fails on the page side.
        ESP_LOGW(TAG, "Protobuf response at %u/%u bytes: low margin",
                 static_cast<unsigned>(os.bytes_written), static_cast<unsigned>(REPLY_CAP));
    }
    return os.bytes_written;
}

size_t buildWifiPb()
{
    char ssid[33];
    bool enabled = false;
    configGetWifi(ssid, sizeof(ssid), nullptr, 0, &enabled);
    Msg msg = Msg_init_zero;
    msg.which_body = Msg_wifi_tag;
    msg.body.wifi.enabled = enabled;
    msg.body.wifi.ssid.funcs.encode = encStr;
    msg.body.wifi.ssid.arg = ssid;
    msg.body.wifi.connected = m_sta_connected.load();
    msg.body.wifi.ip.funcs.encode = encStr;
    msg.body.wifi.ip.arg = m_sta_ip;
    return encodeMsg(msg);   // encode BEFORE ssid goes out of scope
}

// ── History in RAM: same windows (10 min / 1 min / 30 min) but less dense
// sampling — the old 600+360 points produced a ~17 KB JSON when the rings were FULL; with
// 2 simultaneous clients, allocation peaks exhausted the heap (~40 KB free, measured heap_min
// < 11 KB) → malloc failures in the radio internals → freezes/watchdog. ~120 points
// are plenty for the screen (~520 px chart).
constexpr int HIST_FAST_N   = 120;  // 10 min @ 5 s
constexpr int HIST_FAST_DT  = 5;
constexpr int HIST_SPEED_N  = 60;   // 1 min @ 1 s (dedicated speed chart, small)
constexpr int HIST_BATT_N   = 120;  // 30 min @ 15 s
constexpr int HIST_BATT_DT  = 15;
struct
{
    Ring<uint8_t, HIST_FAST_N>  accel, pwml, pwmr, rpml, rpmr;   // rpml/rpmr: wheel rpm (0..250)
    Ring<uint8_t, HIST_SPEED_N> spd;
    Ring<uint8_t, HIST_BATT_N>  batt;
    int tick = 0;
} m_hist;
SemaphoreHandle_t  m_hist_mtx = nullptr;
esp_timer_handle_t m_hist_timer = nullptr;

uint8_t pctU8(float v)   // percentage 0..100 (throttle, PWM)
{
    if (v < 0.f) v = 0.f;
    if (v > 100.f) v = 100.f;
    return static_cast<uint8_t>(v + 0.5f);
}

uint8_t u8x10(float v)   // physical value ×10 (resolution 0.1; clamped 0..25.5 → km/h, V)
{
    float s = v * 10.f;
    if (s < 0.f) s = 0.f;
    if (s > 255.f) s = 255.f;
    return static_cast<uint8_t>(s + 0.5f);
}

// Wheel speed (m/s) → rpm, clamped 0..250 (byte; ~3.3 m/s max before saturation on a 10" wheel).
uint8_t rpmU8(float rpm_in)
{
    float rpm = fabsf(rpm_in) / 5.f;   // 1 byte holds 0..255 → 0..1275 rpm (encoder shaft)
    if (rpm > 255.f) rpm = 255.f;
    return static_cast<uint8_t>(rpm + 0.5f);
}

void histSample(void*)
{
    // Runs in the esp_timer task (priority 22, shared with the Wi-Fi/BT timers):
    // blocking here is FORBIDDEN. Mutex busy (a client is building the JSON) → skip
    // the sample, never mind — the history is indicative.
    if (pdTRUE != xSemaphoreTake(m_hist_mtx, 0)) return;
    KartStatus st;
    if (!statusTrySnapshot(st))   // same rule as m_hist_mtx: status busy → sample skipped
    {
        xSemaphoreGive(m_hist_mtx);
        return;
    }
    m_hist.spd.push(u8x10(fabsf(st.m_speed_ms)));                        // |vehicle v| m/s ×10, each s
    if (0 == (m_hist.tick % HIST_FAST_DT))
    {
        m_hist.accel.push(pctU8(fabsf(st.m_fwd) * 100.f));               // % (forward command)
        m_hist.pwml.push(pctU8(fabsf(st.m_out_l) * 100.f));              // %
        m_hist.pwmr.push(pctU8(fabsf(st.m_out_r) * 100.f));              // %
        m_hist.rpml.push(rpmU8(st.m_rpm_l));                             // rpm left wheel
        m_hist.rpmr.push(rpmU8(st.m_rpm_r));                             // rpm right wheel
    }
    if (0 == (m_hist.tick % HIST_BATT_DT))
    {
        m_hist.batt.push(u8x10(st.m_vbat));                              // V ×10
    }
    ++m_hist.tick;
    xSemaphoreGive(m_hist_mtx);
}

// History: linearize the rings into static buffers then protobuf "bytes"
// encoding (1 byte/sample → ~850 bytes when the rings are full, vs ~17 KB
// for the original JSON). Binary cache rebuilt at most once/s, shared between clients.
uint8_t m_hist_bin[1024];   // measured full ≈ 900 bytes (780 of samples + headers)
size_t  m_hist_bin_len = 0;
int64_t m_hist_bin_us = -1;

const pb_byte_t* buildHistPb(size_t& len)
{
    const int64_t now = esp_timer_get_time();
    if (m_hist_bin_us >= 0 && (now - m_hist_bin_us) < 1000000)
    {
        len = m_hist_bin_len;
        return m_hist_bin;
    }
    static uint8_t lin_fast[5][HIST_FAST_N];
    static uint8_t lin_spd[HIST_SPEED_N];
    static uint8_t lin_batt[HIST_BATT_N];
    BytesArg args[7];
    xSemaphoreTake(m_hist_mtx, portMAX_DELAY);
    const Ring<uint8_t, HIST_FAST_N>* fast[5] = {&m_hist.accel, &m_hist.pwml, &m_hist.pwmr,
                                        &m_hist.rpml, &m_hist.rpmr};
    for (int k = 0; k < 5; ++k)
    {
        args[k] = {lin_fast[k], static_cast<size_t>(fast[k]->copyTo(lin_fast[k], HIST_FAST_N))};
    }
    args[5] = {lin_spd, static_cast<size_t>(m_hist.spd.copyTo(lin_spd, HIST_SPEED_N))};
    args[6] = {lin_batt, static_cast<size_t>(m_hist.batt.copyTo(lin_batt, HIST_BATT_N))};
    xSemaphoreGive(m_hist_mtx);

    Msg msg = Msg_init_zero;
    msg.which_body = Msg_hist_tag;
    Hist& h = msg.body.hist;
    h.dt_fast = HIST_FAST_DT;
    h.dt_spd = 1;
    h.dt_batt = HIST_BATT_DT;
    pb_callback_t* fields[7] = {&h.accel, &h.pwml, &h.pwmr, &h.rpml, &h.rpmr, &h.spd, &h.batt};
    for (int k = 0; k < 7; ++k)
    {
        fields[k]->funcs.encode = encBytes;
        fields[k]->arg = &args[k];
    }
    pb_ostream_t os = pb_ostream_from_buffer(m_hist_bin, sizeof(m_hist_bin));
    if (!pb_encode(&os, Msg_fields, &msg))
    {
        ESP_LOGE(TAG, "Hist encoding: %s", PB_GET_ERROR(&os));
        len = 0;
        return m_hist_bin;
    }
    m_hist_bin_len = os.bytes_written;
    m_hist_bin_us = now;
    len = m_hist_bin_len;
    return m_hist_bin;
}

// Callback: encodes the metadata of ALL parameters (repeated ParamMeta) —
// the strings go directly from the PARAMS table pointers (zero copy).
bool encParamMetas(pb_ostream_t* os, const pb_field_t* field, void* const* arg)
{
    const KartConfig* cfg = static_cast<const KartConfig*>(*arg);
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        const ParamDesc& p = PARAMS[i];
        ParamMeta m = ParamMeta_init_zero;
        m.name.funcs.encode = encStr; m.name.arg = const_cast<char*>(p.name);
        m.desc.funcs.encode = encStr; m.desc.arg = const_cast<char*>(p.desc);
        m.cat.funcs.encode  = encStr; m.cat.arg  = const_cast<char*>(p.cat);
        m.help.funcs.encode = encStr; m.help.arg = const_cast<char*>(p.help);
        const char* type = (PType::Float == p.type) ? "float" : (PType::Int == p.type ? "int" : "bool");
        m.type.funcs.encode = encStr; m.type.arg = const_cast<char*>(type);
        m.min = p.min;
        m.max = p.max;
        m.val = cfg->*(p.field);
        if (!pb_encode_tag_for_field(os, field)) return false;
        if (!pb_encode_submessage(os, ParamMeta_fields, &m)) return false;
    }
    return true;
}

size_t buildConfigPb()
{
    const KartConfig cfg = configSnapshot();
    Msg msg = Msg_init_zero;
    msg.which_body = Msg_config_tag;
    msg.body.config.params.funcs.encode = encParamMetas;
    msg.body.config.params.arg = const_cast<KartConfig*>(&cfg);
    return encodeMsg(msg);
}

// Callback: encodes the VALUES only (repeated ParamVal).
bool encParamVals(pb_ostream_t* os, const pb_field_t* field, void* const* arg)
{
    const KartConfig* cfg = static_cast<const KartConfig*>(*arg);
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        ParamVal v = ParamVal_init_zero;
        snprintf(v.name, sizeof(v.name), "%s", PARAMS[i].name);
        v.val = cfg->*(PARAMS[i].field);
        if (!pb_encode_tag_for_field(os, field)) return false;
        if (!pb_encode_submessage(os, ParamVal_fields, &v)) return false;
    }
    return true;
}

size_t buildValsPb()
{
    const KartConfig cfg = configSnapshot();
    Msg msg = Msg_init_zero;
    msg.which_body = Msg_vals_tag;
    msg.body.vals.params.funcs.encode = encParamVals;
    msg.body.vals.params.arg = const_cast<KartConfig*>(&cfg);
    return encodeMsg(msg);
}

// Battery gauge display scale — decided HERE (the client knows no threshold):
// low = LVC cutoff threshold of the detected type, high = full-charge voltage at rest.
float battDispLo(int bt)
{
    return (24 == bt) ? hw::VBAT24_CUT_V : (12 == bt) ? hw::VBAT12_CUT_V : 0.f;
}
float battDispHi(int bt)
{
    return (24 == bt) ? hw::VBAT24_FULL_V : (12 == bt) ? hw::VBAT12_FULL_V : 0.f;
}

size_t buildStatusPb()
{
    Msg msg = Msg_init_zero;
    msg.which_body = Msg_status_tag;
    const KartStatus s = statusSnapshot();
    Status& st = msg.body.status;
    st.state      = s.m_state;
    st.fault      = s.m_fault;
    st.faults     = s.m_faults;
    st.vbat       = s.m_vbat;
    st.batt_type  = s.m_batt_type;
    st.batt_lo    = battDispLo(s.m_batt_type);
    st.batt_hi    = battDispHi(s.m_batt_type);
    st.speed_ms   = s.m_speed_ms;
    // Wheels in RPM (signed), vehicle in m/s — the conversion happens HERE, on the micro side.
    st.rpm_l      = s.m_rpm_l;   // already in rpm: enc_rpm_per_cps ratio from the controller
    st.rpm_r      = s.m_rpm_r;
    st.fwd        = s.m_fwd;
    st.turn       = s.m_turn;
    st.out_l      = s.m_out_l;
    st.out_r      = s.m_out_r;
    st.brake_mode = s.m_brake_mode;
    st.arming     = s.m_arming;
    st.btn_start  = s.m_btn_start;
    st.pad_conn   = s.m_pad_conn;
    st.pad_batt   = s.m_pad_batt;
    st.pad_x      = s.m_pad_x;
    st.pad_y      = s.m_pad_y;
    st.pad_cx     = s.m_pad_cx;
    st.pad_cy     = s.m_pad_cy;
    st.pad_zl     = s.m_pad_zl;
    st.pad_zr     = s.m_pad_zr;
    st.pad_rx2    = s.m_pad_rx2;
    st.pad_ry2    = s.m_pad_ry2;
    st.pad_btns   = s.m_pad_btns;
    st.pad_age_ms = static_cast<int32_t>(
        std::min<int64_t>((esp_timer_get_time() - input::lastReportUs()) / 1000, 99999));
    return encodeMsg(msg);
}

size_t buildPadPb()
{
    Msg msg = Msg_init_zero;
    msg.which_body = Msg_pad_tag;
    Pad& pd = msg.body.pad;
    pd.conn = statusSnapshot().m_pad_conn;
    pd.name.funcs.encode = encStr;
    pd.name.arg = const_cast<char*>(input::name());
    pd.batt = input::battery();
    pd.calibrated = input::calibrated();
    pd.calstate = input::calState();
    pd.pairing = input::pairing();
    return encodeMsg(msg);
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
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP:return "deep-sleep";
        default:               return "other";
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
        default:                                return "other";
    }
}

// Callback: all IPv6 addresses of an interface (repeated Ip6), arg = esp_netif_t*.
bool encIp6(pb_ostream_t* os, const pb_field_t* field, void* const* arg)
{
    esp_netif_t* netif = static_cast<esp_netif_t*>(*arg);
    if (!netif) return true;
    esp_ip6_addr_t a[5];
    const int n = esp_netif_get_all_ip6(netif, a);
    for (int i = 0; i < n; ++i)
    {
        char addr[48];
        snprintf(addr, sizeof(addr), IPV6STR, IPV62STR(a[i]));
        Ip6 e = Ip6_init_zero;
        e.addr.funcs.encode = encStr; e.addr.arg = addr;
        e.type.funcs.encode = encStr;
        e.type.arg = const_cast<char*>(ip6TypeStr(esp_netif_ip6_get_addr_type(&a[i])));
        if (!pb_encode_tag_for_field(os, field)) return false;
        if (!pb_encode_submessage(os, Ip6_fields, &e)) return false;
    }
    return true;
}

size_t buildSysDynPb()
{
    Msg msg = Msg_init_zero;
    msg.which_body = Msg_sysdyn_tag;
    msg.body.sysdyn.uptime_s  = esp_timer_get_time() / 1000000;
    msg.body.sysdyn.heap_free = esp_get_free_heap_size();
    msg.body.sysdyn.heap_min  = esp_get_minimum_free_heap_size();
    msg.body.sysdyn.ledc_fix  = board::ledcClkFixCount();
    return encodeMsg(msg);
}

size_t buildSysInfoPb()
{
    const esp_app_desc_t* app = esp_app_get_description();

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    uint32_t flash_sz = 0;
    esp_flash_get_size(nullptr, &flash_sz);

    esp_netif_ip_info_t ap_ip{};
    if (m_netif_ap) esp_netif_get_ip_info(m_netif_ap, &ap_ip);

    char ssid[33] = "";
    bool sta_en = false;
    configGetWifi(ssid, sizeof(ssid), nullptr, 0, &sta_en);

    char mac_ap[18], mac_sta[18];
    macTo(mac_ap, WIFI_IF_AP);
    macTo(mac_sta, WIFI_IF_STA);
    char fw_date[32];
    snprintf(fw_date, sizeof(fw_date), "%s %s", app ? app->date : "?", app ? app->time : "");
    char ip_ap[16];
    snprintf(ip_ap, sizeof(ip_ap), IPSTR, IP2STR(&ap_ip.ip));

    Msg msg = Msg_init_zero;
    msg.which_body = Msg_sysinfo_tag;
    SysInfo& si = msg.body.sysinfo;
    auto str = [](pb_callback_t& f, const char* v) {
        f.funcs.encode = encStr;
        f.arg = const_cast<char*>(v);
    };
    str(si.fw_ver,  app ? app->version : "?");
    str(si.fw_proj, app ? app->project_name : "?");
    str(si.fw_date, fw_date);
    str(si.idf,     app ? app->idf_ver : "?");
    str(si.chip,    "ESP32");
    si.cores = chip.cores;
    si.rev = chip.revision;
    si.flash_mb = flash_sz / (1024 * 1024);
    si.uptime_s = esp_timer_get_time() / 1000000;
    si.heap_free = esp_get_free_heap_size();
    si.heap_min = esp_get_minimum_free_heap_size();
    str(si.reset,   resetReasonStr(esp_reset_reason()));
    str(si.mac_ap,  mac_ap);
    str(si.mac_sta, mac_sta);
    str(si.ap_ssid, AP_SSID);
    str(si.ip_ap,   ip_ap);
    si.sta_en = sta_en;
    str(si.sta_ssid, ssid);
    si.sta_conn = m_sta_connected.load();
    str(si.ip_sta,  m_sta_ip);
    si.ip6_ap.funcs.encode  = encIp6;
    si.ip6_ap.arg  = m_netif_ap;
    si.ip6_sta.funcs.encode = encIp6;
    si.ip6_sta.arg = m_netif_sta;
    return encodeMsg(msg);   // encode BEFORE the local buffers go out of scope
}

// LAZY config draft: instantiated (mutex + copy) only if "set" pairs
// actually arrive — not for the 20 Hz status.
struct SetCtx
{
    KartConfig cfg;
    bool       touched = false;
};

// Decoding a ParamVal from "set": applies the value directly to the draft.
bool decSetParam(pb_istream_t* is, const pb_field_t* field, void** arg)
{
    SetCtx* ctx = static_cast<SetCtx*>(*arg);
    ParamVal pv = ParamVal_init_zero;
    if (!pb_decode(is, ParamVal_fields, &pv)) return false;
    if (!ctx->touched)
    {
        ctx->cfg = configSnapshot();
        ctx->touched = true;
    }
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        if (0 == strcmp(PARAMS[i].name, pv.name))
        {
            ctx->cfg.*(PARAMS[i].field) = pv.val;
            break;
        }
    }
    return true;   // unknown name → ignored (client newer than the firmware, etc.)
}

esp_err_t sendGz(httpd_req_t* req, const char* ctype, const uint8_t* s, const uint8_t* e)
{
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, reinterpret_cast<const char*>(s), e - s);
}

esp_err_t rootGet(httpd_req_t* req)  { return sendGz(req, "text/html", index_html_gz_start, index_html_gz_end); }
esp_err_t styleGet(httpd_req_t* req) { return sendGz(req, "text/css", style_css_gz_start, style_css_gz_end); }
esp_err_t chartGet(httpd_req_t* req) { return sendGz(req, "text/javascript", chart_min_js_gz_start, chart_min_js_gz_end); }
esp_err_t pbJsGet(httpd_req_t* req)  { return sendGz(req, "text/javascript", pb_min_js_gz_start, pb_min_js_gz_end); }

// Sends a BINARY (protobuf) frame from a given buffer.
esp_err_t wsSend(httpd_req_t* req, const pb_byte_t* payload, size_t len)
{
    if (0 == len) return ESP_FAIL;   // encoding failed (logged): no empty frame
    httpd_ws_frame_t frame{};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = const_cast<pb_byte_t*>(payload);
    frame.len = len;
    return httpd_ws_send_frame(req, &frame);
}

esp_err_t wsReply(httpd_req_t* req, size_t len)   // sends the m_reply arena
{
    return wsSend(req, m_reply, len);
}

// Static RECEIVE buffer (the httpd task is sequential). Largest request:
// a full "set" ≈ 600 binary bytes (25 name/value pairs).
constexpr size_t BODY_CAP = 1024;
pb_byte_t m_body[BODY_CAP];

esp_err_t wsHandler(httpd_req_t* req)
{
    if (HTTP_GET == req->method)
    {
        return ESP_OK;   // handshake: the client will ask
    }

    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_BINARY;
    if (ESP_OK != httpd_ws_recv_frame(req, &frame, 0))
    {
        return ESP_FAIL;
    }
    if (0 == frame.len || frame.len > BODY_CAP)
    {
        return ESP_OK;
    }
    frame.payload = m_body;
    if (ESP_OK != httpd_ws_recv_frame(req, &frame, frame.len))
    {
        return ESP_FAIL;
    }

    // Decoding the request. "set" applies its name/value pairs as decoding proceeds
    // (decSetParam callback) on a draft instantiated at the first pair.
    SetCtx draft;
    Req rq = Req_init_zero;
    rq.set.funcs.decode = decSetParam;
    rq.set.arg = &draft;
    pb_istream_t is = pb_istream_from_buffer(m_body, frame.len);
    if (!pb_decode(&is, Req_fields, &rq))
    {
        ESP_LOGW(TAG, "Invalid protobuf request: %s", PB_GET_ERROR(&is));
        return ESP_OK;
    }
    const char* cmd = rq.type;

    if (0 == strcmp(cmd, "reboot"))
    {
        Msg msg = Msg_init_zero;
        msg.which_body = Msg_ok_tag;
        wsReply(req, encodeMsg(msg));
        ESP_LOGW(TAG, "Reboot requested via the web");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();   // does not return; on boot → disarmed
        return ESP_OK;
    }
    if (0 == strcmp(cmd, "padunpair")) { input::unpair();       return wsReply(req, buildPadPb()); }
    if (0 == strcmp(cmd, "padpair"))   { input::startPairing(); return wsReply(req, buildPadPb()); }   // ⚠️ erases the calibration
    if (0 == strcmp(cmd, "calstart"))  { input::calStart();     return wsReply(req, buildPadPb()); }
    if (0 == strcmp(cmd, "calfinish")) { input::calFinish();    return wsReply(req, buildPadPb()); }
    if (0 == strcmp(cmd, "calcancel")) { input::calCancel();    return wsReply(req, buildPadPb()); }
    if (0 == strcmp(cmd, "padinfo"))   {                        return wsReply(req, buildPadPb()); }
    if (0 == strcmp(cmd, "wifiset"))
    {
        configSetWifi(rq.ssid, rq.pass, rq.enabled);   // takes effect on reboot
        Msg msg = Msg_init_zero;
        msg.which_body = Msg_ok_tag;
        return wsReply(req, encodeMsg(msg));
    }
    if (0 == strcmp(cmd, "wifiget"))   { return wsReply(req, buildWifiPb()); }
    if (0 == strcmp(cmd, "vals"))      { return wsReply(req, buildValsPb()); }
    if (0 == strcmp(cmd, "sysdyn"))    { return wsReply(req, buildSysDynPb()); }
    if (0 == strcmp(cmd, "hist"))
    {
        size_t n = 0;
        const pb_byte_t* h = buildHistPb(n);
        return wsSend(req, h, n);
    }
    if (0 == strcmp(cmd, "set"))
    {
        if (draft.touched) configUpdate(draft.cfg, true);   // applied by decSetParam
        return wsReply(req, buildValsPb());
    }
    if (0 == strcmp(cmd, "sysinfo"))   { return wsReply(req, buildSysInfoPb()); }
    if (0 == strcmp(cmd, "status"))    { return wsReply(req, buildStatusPb()); }
    return wsReply(req, buildConfigPb());   // "get" (and default)
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

    // Access point (always active)
    wifi_config_t ap{};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = std::strlen(AP_SSID);
    std::strncpy(reinterpret_cast<char*>(ap.ap.password), AP_PASS, sizeof(ap.ap.password));
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode = (std::strlen(AP_PASS) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    // Station (if credentials are saved) → AP+STA mode
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
        ESP_LOGI(TAG, "AP+STA mode: connecting to \"%s\"", ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP \"%s\" ready → http://192.168.4.1", AP_SSID);
}

void webServerStart()
{
    // History in RAM (sampled every second)
    m_hist_mtx = xSemaphoreCreateMutex();
    esp_timer_create_args_t ht{};
    ht.callback = histSample;
    ht.name = "hist";
    ESP_ERROR_CHECK(esp_timer_create(&ht, &m_hist_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(m_hist_timer, 1000000));   // 1 s

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;   // default 4096: too tight for WS + %g formatting (overflow experienced)
    if (ESP_OK != httpd_start(&m_server, &config))
    {
        ESP_LOGE(TAG, "HTTP start failed");
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
    reg("/pb.js",     pbJsGet,  false);
    reg("/ws",        wsHandler, true);
    ESP_LOGI(TAG, "Web + WebSocket server started");
}
