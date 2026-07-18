// config.cpp — Table PARAMS (pointeur-vers-membre) + persistance NVS + accès thread-safe.
#include "config.hpp"

#include <algorithm>
#include <atomic>
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
// N'écrit la clé QUE si la valeur stockée diffère : chaque écriture flash suspend le cache
// (donc la boucle de contrôle, qui s'exécute depuis la flash) et use la NVS pour rien.
void writeFloat(nvs_handle_t handle, const char* key, float value)
{
    float cur;
    if (readFloat(handle, key, cur) && cur == value) return;
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

namespace
{
std::atomic<bool> m_save_pending{false};   // sauvegarde repoussée (kart armé au moment du set)
}

void configUpdate(const KartConfig& cfg, bool persist)
{
    xSemaphoreTake(g_cfg_mtx, portMAX_DELAY);
    g_cfg = cfg;
    g_cfg.clampAll();
    if (persist)
    {
        // Kart ARMÉ → différer l'écriture NVS : les écritures flash suspendent le cache et
        // gèlent la boucle de contrôle plusieurs dizaines de ms — pas pendant la conduite.
        // Les valeurs sont déjà ACTIVES (g_cfg) ; la persistance suivra au désarmement.
        if (g_status.m_arming.load()) m_save_pending.store(true);
        else                          configSave();
    }
    xSemaphoreGive(g_cfg_mtx);
}

// Appelée par la boucle de contrôle au passage armé → désarmé.
void configFlushPending()
{
    if (!m_save_pending.exchange(false)) return;
    xSemaphoreTake(g_cfg_mtx, portMAX_DELAY);
    configSave();
    xSemaphoreGive(g_cfg_mtx);
    ESP_LOGI(TAG, "Réglages différés persistés (kart désarmé)");
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
