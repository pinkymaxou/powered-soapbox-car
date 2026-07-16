// config.cpp — Table PARAMS (pointeur-vers-membre) + persistance NVS + accès thread-safe.
#include "config.hpp"

#include <algorithm>
#include "esp_log.h"
#include "nvs.h"

static const char* TAG = "cfg";

KartConfig        g_cfg;
KartStatus        g_status;
SemaphoreHandle_t g_cfg_mtx = nullptr;

namespace
{
constexpr char NVS_NS[] = "kart";
}

// Source unique de vérité : nom (clé NVS/JSON), libellé, type, min/défaut/max, champ visé.
const ParamDesc PARAMS[] =
{
    {"speed_limit_ms",  "Limite vitesse (m/s)",   PType::Float, 0.3f,  3.3f,   7.f,    &KartConfig::speed_limit_ms},
    // Plafond PWM MANUEL (plancher de sécurité, le plus restrictif gagne) — le plafond
    // AUTOMATIQUE 12 V/Vbat mesurée (ctl::dutyCapVolts) s'applique EN PLUS. Clé renommée
    // (ex-duty_cap_frac, défaut 0,50 pensé pour un pack 20 V fixe) : tout le monde repart
    // du défaut 1,0 = « laisser faire l'automatique ».
    {"duty_cap",        "Plafond PWM manuel (0-1)", PType::Float, 0.05f, 1.0f,   1.f,    &KartConfig::duty_cap_frac},
    {"thr_deadzone",    "Zone morte manche",       PType::Float, 0.f,   0.06f,  0.30f,  &KartConfig::thr_deadzone},
    {"thr_ramp_per_s",  "Douceur avance (D/s)",    PType::Float, 0.2f,  2.f,    20.f,   &KartConfig::thr_ramp_per_s},
    {"vbat_div_ratio",  "Ratio diviseur Vbat",     PType::Float, 1.f,   7.667f, 20.f,   &KartConfig::vbat_div_ratio},
    // Plages élargies 8–29 V : batterie moto 12 V (option probable), pack outil 20 V,
    // ou 24 V (2×12 V série, driver max 30 V). Régler les seuils selon la chimie.
    {"vbat_warn_v",     "Vbat avertissement (V)",  PType::Float, 8.f,   16.5f,  29.f,   &KartConfig::vbat_warn_v},
    {"vbat_cut_v",      "Vbat coupure LVC (V)",    PType::Float, 8.f,   15.0f,  28.f,   &KartConfig::vbat_cut_v},
    {"vbat_recover_v",  "Vbat rearmement (V)",     PType::Float, 8.f,   16.0f,  28.5f,  &KartConfig::vbat_recover_v},
    {"cell_count",      "Cellules Li-ion (S)",     PType::Int,   1.f,   5.f,    14.f,   &KartConfig::cell_count},
    // PID en m/s (clés renommées : l'erreur a changé d'unité km/h→m/s, défauts ×3,6 — les
    // anciennes clés NVS pid_*/vmax_* sont ignorées, les nouvelles partent des bons défauts).
    {"brk_kp",          "Frein PID Kp (m/s)",      PType::Float, 0.f,   0.43f,  5.f,    &KartConfig::pid_kp},
    {"brk_ki",          "Frein PID Ki (m/s)",      PType::Float, 0.f,   0.29f,  10.f,   &KartConfig::pid_ki},
    {"brk_kd",          "Frein PID Kd (m/s)",      PType::Float, 0.f,   0.011f, 2.f,    &KartConfig::pid_kd},
    {"vlim_kp",         "Limiteur PID Kp (m/s)",   PType::Float, 0.f,   0.54f,  5.f,    &KartConfig::vmax_kp},
    {"vlim_ki",         "Limiteur PID Ki (m/s)",   PType::Float, 0.f,   0.50f,  10.f,   &KartConfig::vmax_ki},
    {"vlim_kd",         "Limiteur PID Kd (m/s)",   PType::Float, 0.f,   0.f,    2.f,    &KartConfig::vmax_kd},
    {"turn_gain",       "Gain de virage (0-1)",    PType::Float, 0.f,   0.6f,   1.f,    &KartConfig::turn_gain},
    // Anti-renversement « rampe » : virage ±100 % sous turn_full_ms, décroît linéairement
    // jusqu'à turn_hi à speed_limit_ms (vitesse MESURÉE). Recul plafonné à rev_limit.
    {"turn_full_ms",    "Virage 100% sous (m/s)",  PType::Float, 0.1f,  0.5f,   3.f,    &KartConfig::turn_full_ms},
    {"turn_hi",         "Virage max a Vmax (0-1)", PType::Float, 0.1f,  0.5f,   1.f,    &KartConfig::turn_hi},
    {"rev_limit",       "Limite recul (0-1)",      PType::Float, 0.f,   0.5f,   1.f,    &KartConfig::rev_limit},
    {"turn_rate",       "Douceur virage (D/s)",    PType::Float, 0.3f,  3.0f,   20.f,   &KartConfig::turn_rate},
    {"use_encoders",    "Utiliser encodeurs (0/1)", PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::use_encoders},
    {"allow_reverse",   "Marche arriere (0/1)",    PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::allow_reverse},
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
    const esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    if (ESP_OK == err) ESP_LOGI(TAG, "Réglages sauvegardés");
    return ESP_OK == err;
}

KartConfig configSnapshot()
{
    xSemaphoreTake(g_cfg_mtx, portMAX_DELAY);
    const KartConfig cfg = g_cfg;
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
