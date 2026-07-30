// config.cpp — NVS persistence + thread-safe config/status access (PARAMS table: config_params.cpp).
#include "config.hpp"

#include <algorithm>
#include "esp_log.h"
#include "nvs.h"

static const char* TAG = "cfg";

KartConfig        g_cfg;
SemaphoreHandle_t g_cfg_mtx = nullptr;

namespace
{
KartStatus        g_status;               // shared state — access ONLY via the functions
SemaphoreHandle_t g_status_mtx = nullptr; // created by configInit()
}

KartStatus statusSnapshot()
{
    xSemaphoreTake(g_status_mtx, portMAX_DELAY);
    const KartStatus copy = g_status;
    xSemaphoreGive(g_status_mtx);
    return copy;
}

bool statusTrySnapshot(KartStatus& out)
{
    if (pdTRUE != xSemaphoreTake(g_status_mtx, 0)) return false;
    out = g_status;
    xSemaphoreGive(g_status_mtx);
    return true;
}

void statusPublish(const KartStatus& s)
{
    xSemaphoreTake(g_status_mtx, portMAX_DELAY);
    g_status = s;
    xSemaphoreGive(g_status_mtx);
}

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
// Writes the key ONLY if the stored value differs: each flash write suspends the cache
// (thus the control loop, which runs from flash) and wears the NVS for nothing.
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
    g_status_mtx = xSemaphoreCreateMutex();
    if (!configLoad())
    {
        ESP_LOGW(TAG, "No settings in NVS → default values");
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
        // NVS keeps the historical float-blob format; cfgSet narrows to int for int/bool params.
        float v;
        if (readFloat(handle, PARAMS[i].name, v)) { cfgSet(cfg, PARAMS[i], v); any = true; }
    }
    nvs_close(handle);
    cfg.clampAll();
    g_cfg = cfg;
    if (any) ESP_LOGI(TAG, "Settings loaded from NVS");
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
        writeFloat(handle, PARAMS[i].name, cfgGet(g_cfg, PARAMS[i]));
    }
    const esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    if (ESP_OK == err) ESP_LOGI(TAG, "Settings saved");
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
    // No armed check here anymore: the web "set" handler REFUSES the whole request while
    // the kart is armed, so this only ever runs disarmed. That retired the old deferred-save
    // machinery (values applied live, NVS flushed by the control task on the disarm edge) —
    // changing control parameters mid-drive was a dubious feature to begin with, and the
    // flash write it deferred (cache suspended = every flash-resident task frozen, control
    // loop included) now simply cannot coincide with driving.
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
    ESP_LOGI(TAG, "Wi-Fi station %s (SSID=%s)", enabled ? "enabled" : "disabled", ssid ? ssid : "");
}
