// config.cpp — Table PARAMS (pointeur-vers-membre) + persistance NVS + accès thread-safe.
#include "config.hpp"

#include <algorithm>
#include "esp_log.h"
#include "nvs.h"

static const char* TAG = "cfg";

KartConfig        g_cfg;
KartStatus        g_status;
SemaphoreHandle_t g_cfg_mtx = nullptr;

namespace { constexpr char NVS_NS[] = "kart"; }

// Source unique de vérité : nom (clé NVS/JSON), libellé, type, min/défaut/max, champ visé.
const ParamDesc PARAMS[] =
{
    {"speed_limit_kmh", "Limite vitesse (km/h)",  PType::Float, 1.f,   12.f,   25.f,   &KartConfig::speed_limit_kmh},
    {"duty_cap_frac",   "Plafond PWM (0-1)",       PType::Float, 0.05f, 0.50f,  1.f,    &KartConfig::duty_cap_frac},
    {"thr_deadzone",    "Zone morte accel.",       PType::Float, 0.f,   0.06f,  0.30f,  &KartConfig::thr_deadzone},
    {"thr_top_margin",  "Marge haute accel.",      PType::Float, 0.f,   0.05f,  0.30f,  &KartConfig::thr_top_margin},
    {"thr_ramp_per_s",  "Rampe accel. (1/s)",      PType::Float, 0.2f,  2.f,    20.f,   &KartConfig::thr_ramp_per_s},
    {"thr_min_raw",     "Brut accel. min",         PType::Int,   0.f,   200.f,  4095.f, &KartConfig::thr_min_raw},
    {"thr_max_raw",     "Brut accel. max",         PType::Int,   0.f,   3000.f, 4095.f, &KartConfig::thr_max_raw},
    {"vbat_div_ratio",  "Ratio diviseur Vbat",     PType::Float, 1.f,   7.667f, 20.f,   &KartConfig::vbat_div_ratio},
    {"vbat_warn_v",     "Vbat avertissement (V)",  PType::Float, 12.f,  16.5f,  21.f,   &KartConfig::vbat_warn_v},
    {"vbat_cut_v",      "Vbat coupure LVC (V)",    PType::Float, 13.5f, 15.0f,  20.f,   &KartConfig::vbat_cut_v},
    {"vbat_recover_v",  "Vbat rearmement (V)",     PType::Float, 12.f,  16.0f,  20.5f,  &KartConfig::vbat_recover_v},
    {"cell_count",      "Cellules Li-ion (S)",     PType::Int,   1.f,   5.f,    14.f,   &KartConfig::cell_count},
    {"pid_kp",          "Frein PID Kp",            PType::Float, 0.f,   0.120f, 2.f,    &KartConfig::pid_kp},
    {"pid_ki",          "Frein PID Ki",            PType::Float, 0.f,   0.080f, 5.f,    &KartConfig::pid_ki},
    {"pid_kd",          "Frein PID Kd",            PType::Float, 0.f,   0.003f, 1.f,    &KartConfig::pid_kd},
    {"vmax_kp",         "Limiteur PID Kp",         PType::Float, 0.f,   0.150f, 2.f,    &KartConfig::vmax_kp},
    {"vmax_ki",         "Limiteur PID Ki",         PType::Float, 0.f,   0.140f, 5.f,    &KartConfig::vmax_ki},
    {"vmax_kd",         "Limiteur PID Kd",         PType::Float, 0.f,   0.f,    1.f,    &KartConfig::vmax_kd},
    {"allow_reverse",   "Marche arriere (0/1)",    PType::Bool,  0.f,   0.f,    1.f,    &KartConfig::allow_reverse},
    {"arm_hold_ms",     "Appui armement (ms)",     PType::Int,   200.f, 1000.f, 5000.f, &KartConfig::arm_hold_ms},
    {"disarm_s",        "Desarmement auto (s)",    PType::Int,   5.f,   30.f,   600.f,  &KartConfig::disarm_s},
    {"led_count",       "Nb LEDs ruban",           PType::Int,   1.f,   10.f,   60.f,   &KartConfig::led_count},
    {"led_brightness",  "Luminosite LEDs",         PType::Int,   1.f,   64.f,   255.f,  &KartConfig::led_brightness},
};
const int PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

void KartConfig::setDefaults()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        this->*(PARAMS[i].field) = PARAMS[i].def;
    }
}

void KartConfig::clampAll()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        float& f = this->*(PARAMS[i].field);
        f = std::clamp(f, PARAMS[i].min, PARAMS[i].max);
    }
    // Cohérence des seuils LVC : cut < recover ≤ warn
    if (vbat_recover_v < vbat_cut_v + 0.2f) vbat_recover_v = vbat_cut_v + 0.2f;
    if (vbat_warn_v < vbat_recover_v)        vbat_warn_v = vbat_recover_v;
}

namespace
{
bool readFloat(nvs_handle_t handle, const char* key, float& out)
{
    float tmp;
    size_t size = sizeof(tmp);
    if (ESP_OK == nvs_get_blob(handle, key, &tmp, &size) && sizeof(tmp) == size)
    {
        out = tmp;
        return true;
    }
    return false;
}
void writeFloat(nvs_handle_t handle, const char* key, float value)
{
    nvs_set_blob(handle, key, &value, sizeof(value));
}
} // namespace

void configInit()
{
    g_cfg_mtx = xSemaphoreCreateMutex();
    if (!configLoad())
    {
        ESP_LOGW(TAG, "Aucun réglage en NVS → valeurs par défaut");
        g_cfg.setDefaults();
        configSave();
    }
}

bool configLoad()
{
    KartConfig cfg;
    cfg.setDefaults();
    nvs_handle_t handle;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READONLY, &handle))
    {
        g_cfg = cfg;
        return false;
    }
    bool any = false;
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        any |= readFloat(handle, PARAMS[i].name, cfg.*(PARAMS[i].field));
    }
    nvs_close(handle);
    cfg.clampAll();
    g_cfg = cfg;
    if (any) ESP_LOGI(TAG, "Réglages chargés depuis la NVS");
    return any;
}

bool configSave()
{
    nvs_handle_t handle;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READWRITE, &handle))
    {
        return false;
    }
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        writeFloat(handle, PARAMS[i].name, g_cfg.*(PARAMS[i].field));
    }
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    if (ESP_OK == err) ESP_LOGI(TAG, "Réglages sauvegardés");
    return ESP_OK == err;
}

KartConfig configSnapshot()
{
    xSemaphoreTake(g_cfg_mtx, portMAX_DELAY);
    KartConfig cfg = g_cfg;
    xSemaphoreGive(g_cfg_mtx);
    return cfg;
}

void configUpdate(const KartConfig& cfg, bool persist)
{
    xSemaphoreTake(g_cfg_mtx, portMAX_DELAY);
    g_cfg = cfg;
    g_cfg.clampAll();
    if (persist) configSave();
    xSemaphoreGive(g_cfg_mtx);
}

bool configGetWifi(char* ssid, size_t ssid_size, char* pass, size_t pass_size, bool* enabled)
{
    ssid[0] = '\0';
    if (pass) pass[0] = '\0';
    if (enabled) *enabled = false;
    nvs_handle_t handle;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READONLY, &handle)) return false;
    size_t sl = ssid_size;
    esp_err_t err = nvs_get_str(handle, "sta_ssid", ssid, &sl);
    if (pass)
    {
        size_t pl = pass_size;
        nvs_get_str(handle, "sta_pass", pass, &pl);
    }
    uint8_t en = 0;
    nvs_get_u8(handle, "sta_en", &en);
    nvs_close(handle);
    if (enabled) *enabled = (0 != en);
    return ESP_OK == err && '\0' != ssid[0] && 0 != en;
}

void configSetWifi(const char* ssid, const char* pass, bool enabled)
{
    nvs_handle_t handle;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READWRITE, &handle)) return;
    nvs_set_str(handle, "sta_ssid", ssid ? ssid : "");
    nvs_set_str(handle, "sta_pass", pass ? pass : "");
    nvs_set_u8(handle, "sta_en", enabled ? 1 : 0);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Wi-Fi station %s (SSID=%s)", enabled ? "activé" : "désactivé", ssid ? ssid : "");
}
