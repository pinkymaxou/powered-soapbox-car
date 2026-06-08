// config.hpp — Configuration : struct à champs nommés + table PARAMS pointant
// vers chaque champ (pointeur-vers-membre). Accès ergonomique (cfg.speed_limit_kmh)
// ET traitement générique (NVS / web / validation) en itérant PARAMS.
#pragma once

#include <atomic>
#include <cstdint>
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Arrondi float → int (les champs entiers/bool sont stockés en float).
inline int iround(float v) { return static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f); }

// ───────────────────────── Constantes matérielles (compile-time) ─────────────────────────
namespace hw
{
constexpr int   CTRL_HZ       = 500;   // boucle d'asservissement (FreeRTOS tick 1000 Hz)
constexpr int   CTRL_DT_MS    = 1000 / CTRL_HZ;
constexpr float CTRL_DT_S     = 1.0f / CTRL_HZ;
constexpr int   WDT_TIMEOUT_S = 5;   // aussi réglé via sdkconfig (reboot si blocage > 5 s)

constexpr int               PWM_FREQ_HZ = 18000;
constexpr ledc_timer_bit_t  PWM_RES     = LEDC_TIMER_12_BIT;
constexpr int               PWM_MAX     = 4095;

constexpr int ADC_OVERSAMPLE = 16;

// Capteur d'angle AS5600 (I2C, 12 bits absolu = 4096 counts/tour). Cinématique CONNUE :
// AS5600 = 1 tour / 16 tours moteur (= sortie gearbox), puis courroie 1:1 jusqu'à la roue
// → le capteur tourne EXACTEMENT à la vitesse roue ⇒ GEAR_RATIO = 1. Roue 12" = 0,3048 m.
// Alim 3,3 V natif → AUCUN level-shift. Vitesse = dérivée de l'angle (Δcounts × CTRL_HZ) avec
// gestion du wrap 0↔4095 ; le SIGNE de Δ donne le sens. À 500 Hz : aucune ambiguïté.
constexpr float AS5600_CPR    = 4096.0f;  // counts par tour (12 bits)
constexpr float GEAR_RATIO    = 1.0f;     // capteur ≡ vitesse roue (gearbox 1:16 puis courroie 1:1)
constexpr float WHEEL_DIAM_M  = 0.3048f;  // roue 12" (connue)

constexpr int     I2C_FREQ_HZ       = 400000;  // Fast-mode (le capteur supporte jusqu'à 1 MHz)
constexpr uint8_t AS5600_ADDR       = 0x36;    // adresse I2C fixe (un seul capteur par bus)
constexpr uint8_t AS5600_REG_RAWANG = 0x0C;    // RAW ANGLE 12 bits (octets 0x0C MSB / 0x0D LSB)

constexpr float THR_REST_FRAC        = 0.05f;
constexpr int   THR_FAULT_RAW_LOW    = 60;
constexpr int   THR_FAULT_RAW_HIGH   = 4030;
constexpr int   THR_MIN_VALID_SPAN   = 600;
constexpr float THR_BRAKE_RAMP_PER_S = 6.0f;
constexpr int   VBAT_SAG_DEBOUNCE_MS = 500;
constexpr int   LVC_POWEROFF_MS      = 30000;  // coupure auto (powerOff) après 30 s sous le seuil
constexpr float VBAT_WARN_DERATE     = 0.6f;
constexpr float REVERSE_FACTOR       = 0.5f;
constexpr float EBRAKE_MIN_KMH       = 0.5f;
constexpr float ENC_STUCK_PWM        = 0.10f;
constexpr int   ENC_STUCK_MS         = 1000;
constexpr int   BTN_DEBOUNCE_TICKS   = 3;
} // namespace hw

// ───────────────────────── Configuration (champs nommés, persistée) ─────────────────────────
// Tout est stocké en float (les entiers/bool aussi) → pointeur-vers-membre homogène.
struct KartConfig
{
    float speed_limit_kmh;
    float duty_cap_frac;
    float thr_deadzone;
    float thr_top_margin;
    float thr_ramp_per_s;
    float thr_min_raw;
    float thr_max_raw;
    float vbat_div_ratio;
    float vbat_warn_v;
    float vbat_cut_v;
    float vbat_recover_v;
    float cell_count;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float vmax_kp;
    float vmax_ki;
    float vmax_kd;
    float allow_reverse;
    float arm_hold_ms;
    float disarm_s;
    float led_count;
    float led_brightness;

    void setDefaults();
    void clampAll();
};

enum class PType : uint8_t { Float, Int, Bool };

struct ParamDesc
{
    const char*        name;   // clé NVS + clé JSON (≤ 15 caractères pour NVS)
    const char*        desc;   // libellé pour la page web
    PType              type;
    float              min, def, max;
    float KartConfig::* field; // pointeur vers le champ correspondant
};

extern const ParamDesc PARAMS[];
extern const int       PARAM_COUNT;

// ───────────────────────── Télémétrie ─────────────────────────
enum class State : int { Lockout = 0, Calibrate = 1, Run = 2, Fault = 3 };
enum class Fault : int { None = 0, EStop = 1, Lvc = 2, Throttle = 3, NotCalibrated = 4, Encoder = 5 };

struct KartStatus
{
    std::atomic<int>   m_state{static_cast<int>(State::Lockout)};
    std::atomic<int>   m_fault{static_cast<int>(Fault::None)};
    std::atomic<int>   m_thr_raw{0};
    std::atomic<float> m_throttle{0.f};
    std::atomic<float> m_vbat{0.f};
    std::atomic<float> m_speed{0.f};   // vitesse unique (capteur AS5600 sur l'essieu)
    std::atomic<bool>  m_btn_start{false};
    std::atomic<bool>  m_btn_rev{false};
    std::atomic<float> m_out_l{0.f};
    std::atomic<float> m_out_r{0.f};
    std::atomic<bool>  m_brake{false};
    std::atomic<bool>  m_rev_led{false};
    std::atomic<bool>  m_arming{false};
    std::atomic<bool>  m_estop{false};
    std::atomic<int>   m_cmd{0};
};

// ───────────────────────── Globaux ─────────────────────────
extern KartConfig        g_cfg;
extern KartStatus        g_status;
extern SemaphoreHandle_t g_cfg_mtx;

void       configInit();
bool       configLoad();
bool       configSave();
KartConfig configSnapshot();
void       configUpdate(const KartConfig& c, bool persist);

bool configGetWifi(char* ssid, size_t ssid_size, char* pass, size_t pass_size, bool* enabled = nullptr);
void configSetWifi(const char* ssid, const char* pass, bool enabled);
