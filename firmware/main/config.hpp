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
inline int iround(float v)
{
    return static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f);
}

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

constexpr int ADC_OVERSAMPLE = 8;   // nb de lectures ADS1115 moyennées (lisse le résidu de bruit)

// Convertisseur A/N externe ADS1115 (16 bits, I2C) — remplace l'ADC interne de l'ESP32.
// Sur le bus 0 (avec l'AS5600 gauche). Adresse réglée par ADDR ; 0x48 = ADDR→GND.
constexpr uint8_t ADS1115_ADDR = 0x48;

// Capteur d'angle AS5600 (I2C, 12 bits absolu = 4096 counts/tour). Cinématique (voir
// doc/reducteur.md) : aimant sur la SORTIE DE BOÎTE (1:8), puis courroie 1:2 jusqu'à la roue
// → le capteur fait 2 tours par tour de roue ⇒ GEAR_RATIO = 2. Roue 10" = 0,254 m.
// Alim 3,3 V natif → AUCUN level-shift. Vitesse = dérivée de l'angle (Δcounts × CTRL_HZ) avec
// gestion du wrap 0↔4095 ; le SIGNE de Δ donne le sens. À 500 Hz : aucune ambiguïté.
constexpr float AS5600_CPR    = 4096.0f;  // counts par tour (12 bits)
constexpr float GEAR_RATIO    = 2.0f;     // tours capteur par tour de roue (sortie de boîte, courroie 1:2)
constexpr float WHEEL_DIAM_M  = 0.254f;   // roue 10"
constexpr float TRACK_M       = 0.84f;    // voie avant (m) — estime le lacet ω ≈ (vD−vG)/voie

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
constexpr float EBRAKE_MIN_MPS       = 0.15f;  // en-dessous, on considère la roue arrêtée (frein PID)
constexpr float ENC_STUCK_PWM        = 0.10f;
constexpr int   ENC_STUCK_MS         = 1000;
// Lissage vitesse (moyenne exponentielle) : à 500 Hz le Δangle par tick est quantifié
// (~0,05 m/s par count avec GEAR_RATIO 2 / roue 10"). α ~0,25 → cte de temps ~4 ticks (8 ms).
constexpr float SPEED_EMA_ALPHA      = 0.25f;
constexpr int   BTN_DEBOUNCE_TICKS   = 3;
} // namespace hw

// ───────────────────────── Configuration (champs nommés, persistée) ─────────────────────────
// Tout est stocké en float (les entiers/bool aussi) → pointeur-vers-membre homogène.
struct KartConfig
{
    float speed_limit_ms;   // limite de vitesse VÉHICULE (m/s)
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
    float turn_gain;     // part du différentiel à fond de manche X (0..1)
    float a_lat_max;     // accélération latérale max tolérée (m/s²) — anti-renversement
    float turn_rate;     // pente max du virage (Δ/s) — adoucit les coups de manche brusques
    float use_encoders;  // 1 = asservissement vitesse/frein/défaut via AS5600 ; 0 = ignore les encodeurs
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
    std::atomic<float> m_vbat{0.f};
    std::atomic<float> m_speed_l{0.f};   // vitesse roue avant gauche SIGNÉE (AS5600 #1, m/s)
    std::atomic<float> m_speed_r{0.f};   // vitesse roue avant droite SIGNÉE (AS5600 #2, m/s)
    std::atomic<float> m_speed_ms{0.f};  // vitesse VÉHICULE signée (m/s) = (vG+vD)/2 — pivot sur place → 0
    std::atomic<float> m_fwd{0.f};       // consigne d'avance après mix/limites [-1..1]
    std::atomic<float> m_turn{0.f};      // consigne de virage après anti-renversement [-1..1]
    std::atomic<bool>  m_btn_start{false};
    std::atomic<float> m_out_l{0.f};     // PWM moteur gauche [-1..1]
    std::atomic<float> m_out_r{0.f};     // PWM moteur droite [-1..1]
    std::atomic<bool>  m_brake{false};
    std::atomic<bool>  m_arming{false};
    std::atomic<bool>  m_estop{false};
    std::atomic<bool>  m_pad_conn{false}; // manette connectée
    std::atomic<int>   m_pad_batt{-1};    // batterie manette 0..100 (-1 inconnu)
    std::atomic<float> m_pad_x{0.f};      // stick BRUT virage [-1..1] (position physique, cercle)
    std::atomic<float> m_pad_y{0.f};      // stick BRUT avance [-1..1] (position physique, cercle)
    std::atomic<float> m_pad_cx{0.f};     // consigne virage COMPENSÉE cercle→carré [-1..1]
    std::atomic<float> m_pad_cy{0.f};     // consigne avance COMPENSÉE cercle→carré [-1..1]
    std::atomic<float> m_pad_zl{0.f};     // gâchette analogique gauche ZL [0..1] (affichage)
    std::atomic<float> m_pad_zr{0.f};     // gâchette analogique droite ZR [0..1] (affichage)
    std::atomic<float> m_pad_rx2{0.f};    // stick DROIT X [-1..1] (affichage seulement)
    std::atomic<float> m_pad_ry2{0.f};    // stick DROIT Y [-1..1] (affichage seulement)
    std::atomic<unsigned> m_pad_btns{0};  // masque : boutons | (misc<<16) | (dpad<<24) (affichage)
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
