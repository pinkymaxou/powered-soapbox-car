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

// Source unique de vérité : nom (clé NVS/JSON), libellé, catégorie (regroupement visuel),
// aide (infobulle au survol — SANS guillemets doubles, injectée telle quelle dans le JSON),
// type, min/défaut/max, champ visé. L'ordre du tableau = l'ordre d'affichage : garder les
// entrées d'une même catégorie CONSÉCUTIVES (la page web groupe les cat identiques qui se suivent).
// NB : les seuils LVC ne sont PAS ici — batterie 12 V ou 24 V détectée au démarrage,
// seuils codés en dur par type (hw::VBAT12_*/VBAT24_*).
const ParamDesc PARAMS[] =
{
    {"speed_limit_ms",  "Limite vitesse (m/s)",   "Vitesse et puissance",
     "Vitesse maximale du vehicule en m/s. Au-dela, le limiteur PID retient le kart (encodeurs requis). C'est aussi la vitesse ou le virage est le plus bride par l'anti-renversement.",
     PType::Float, 0.3f,  3.3f,   7.f,    &KartConfig::speed_limit_ms},
    // Plafond PWM MANUEL (le plus restrictif gagne) — le plafond AUTOMATIQUE 12 V/Vbat
    // mesurée (ctl::dutyCapVolts) s'applique EN PLUS. Défaut 1,0 = laisser faire l'auto.
    {"duty_cap",        "Plafond PWM manuel (0-1)", "Vitesse et puissance",
     "Plafond PWM fixe, en plus du plafond automatique 12 V / tension batterie mesuree (le plus restrictif gagne). Laisser 1,0 en usage normal ; a baisser a la main si on roule sans capteur de tension sur plus de 12 V.",
     PType::Float, 0.05f, 1.0f,   1.f,    &KartConfig::duty_cap_frac},
    {"thr_deadzone",    "Zone morte manche",       "Manette",
     "Rayon autour du centre du stick ou la position est ignoree (0-0,3). Compense un stick qui ne revient pas exactement au centre.",
     PType::Float, 0.f,   0.06f,  0.30f,  &KartConfig::thr_deadzone},
    {"thr_ramp_per_s",  "Douceur avance (D/s)",    "Manette",
     "Pente maximale de la consigne d'avance (pleine echelle par seconde). Plus petit = departs et arrets plus doux.",
     PType::Float, 0.2f,  2.f,    20.f,   &KartConfig::thr_ramp_per_s},
    {"turn_gain",       "Gain de virage (0-1)",    "Manette",
     "Part du differentiel gauche/droite a fond de stick. 0,6 = pivot sur place plafonne a 60 % de la puissance.",
     PType::Float, 0.f,   0.6f,   1.f,    &KartConfig::turn_gain},
    {"turn_rate",       "Douceur virage (D/s)",    "Manette",
     "Pente maximale de la consigne de virage (pleine echelle par seconde). Adoucit les coups de stick brusques.",
     PType::Float, 0.3f,  3.0f,   20.f,   &KartConfig::turn_rate},
    // Anti-renversement « rampe » : virage ±100 % sous turn_full_ms, décroît linéairement
    // jusqu'à turn_hi à speed_limit_ms (vitesse MESURÉE). Recul plafonné à rev_limit.
    {"turn_full_ms",    "Virage 100% sous (m/s)",  "Anti-renversement",
     "Sous cette vitesse vehicule (m/s), le virage est autorise a 100 % (pivot sur place permis). Au-dela, la limite descend lineairement jusqu'a la limite Vmax.",
     PType::Float, 0.1f,  0.5f,   3.f,    &KartConfig::turn_full_ms},
    {"turn_hi",         "Virage max a Vmax (0-1)", "Anti-renversement",
     "Limite de virage atteinte a la vitesse maximale. 0,5 = a fond de vitesse on ne peut braquer qu'a 50 % — empeche le renversement.",
     PType::Float, 0.1f,  0.5f,   1.f,    &KartConfig::turn_hi},
    {"rev_limit",       "Limite recul (0-1)",      "Anti-renversement",
     "Plafond de puissance en marche arriere. 0,5 = on recule au maximum a 50 % — evite de reculer dangereusement vite.",
     PType::Float, 0.f,   0.5f,   1.f,    &KartConfig::rev_limit},
    {"vbat_div_ratio",  "Ratio diviseur Vbat",     "Batterie",
     "Rapport du pont diviseur de mesure de tension : Vbat = tension lue par l'ADS1115 x ce ratio. A calibrer au multimetre. Le type de batterie (12 ou 24 V) et les seuils de coupure sont detectes automatiquement au demarrage.",
     PType::Float, 1.f,   7.667f, 20.f,   &KartConfig::vbat_div_ratio},
    // PID en m/s (l'erreur est en m/s depuis le passage km/h→m/s).
    {"brk_kp",          "Frein PID Kp (m/s)",      "Asservissement (PID)",
     "Gain proportionnel du frein electrique actif (consigne vitesse 0). Encodeurs requis.",
     PType::Float, 0.f,   0.43f,  5.f,    &KartConfig::pid_kp},
    {"brk_ki",          "Frein PID Ki (m/s)",      "Asservissement (PID)",
     "Gain integral du frein electrique actif : rattrape une pente qui fait glisser le kart a l'arret.",
     PType::Float, 0.f,   0.29f,  10.f,   &KartConfig::pid_ki},
    {"brk_kd",          "Frein PID Kd (m/s)",      "Asservissement (PID)",
     "Gain derive du frein electrique actif : amortit les oscillations de freinage.",
     PType::Float, 0.f,   0.011f, 2.f,    &KartConfig::pid_kd},
    {"vlim_kp",         "Limiteur PID Kp (m/s)",   "Asservissement (PID)",
     "Gain proportionnel du limiteur de vitesse (retient le kart a la limite de vitesse).",
     PType::Float, 0.f,   0.54f,  5.f,    &KartConfig::vmax_kp},
    {"vlim_ki",         "Limiteur PID Ki (m/s)",   "Asservissement (PID)",
     "Gain integral du limiteur de vitesse : tient la limite en descente.",
     PType::Float, 0.f,   0.50f,  10.f,   &KartConfig::vmax_ki},
    {"vlim_kd",         "Limiteur PID Kd (m/s)",   "Asservissement (PID)",
     "Gain derive du limiteur de vitesse : amortit les oscillations autour de la limite.",
     PType::Float, 0.f,   0.f,    2.f,    &KartConfig::vmax_kd},
    {"use_encoders",    "Utiliser encodeurs (0/1)", "Comportement",
     "1 = les capteurs AS5600 servent au limiteur de vitesse, au frein PID, a l'anti-renversement et au defaut capteur bloque. 0 = banc d'essai sans encodeurs cables.",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::use_encoders},
    {"allow_reverse",   "Marche arriere (0/1)",    "Comportement",
     "Autoriser la marche arriere (toujours plafonnee par la limite de recul).",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::allow_reverse},
    {"arm_hold_ms",     "Appui armement (ms)",     "Comportement",
     "Duree d'appui maintenu sur START (physique ou manette) pour armer, stick centre exige.",
     PType::Int,   200.f, 1000.f, 5000.f, &KartConfig::arm_hold_ms},
    {"disarm_s",        "Desarmement auto (s)",    "Comportement",
     "Desarmement automatique apres ce delai sans toucher au stick.",
     PType::Int,   5.f,   30.f,   600.f,  &KartConfig::disarm_s},
    {"led_count",       "Nb LEDs ruban",           "LEDs",
     "Nombre de LEDs WS2812 sur le ruban d'etat.",
     PType::Int,   1.f,   10.f,   60.f,   &KartConfig::led_count},
    {"led_brightness",  "Luminosite LEDs",         "LEDs",
     "Luminosite du ruban (1-255).",
     PType::Int,   1.f,   64.f,   255.f,  &KartConfig::led_brightness},
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
    // (Les seuils LVC ne sont plus des paramètres : codés en dur selon la batterie
    // 12/24 V détectée au démarrage — hw::VBAT12_*/VBAT24_*, cohérence garantie.)
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
