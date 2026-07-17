// config.hpp — Configuration : struct à champs nommés + table PARAMS pointant
// vers chaque champ (pointeur-vers-membre). Accès ergonomique (cfg.speed_limit_ms)
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

// Moteurs 12 V nominaux, driver 6–30 V : le duty est plafonné AUTOMATIQUEMENT à
// MOTOR_V_NOM / Vbat mesurée (12 V → ~100 %, 20 V → ~60 %, 24 V → ~50 %), voir
// ctl::dutyCapVolts. Vbat est lissée lentement (τ ≈ 1 s à 500 Hz) : sans ce filtrage,
// l'affaissement sous charge ferait osciller le plafond (sag → Vbat baisse → duty monte).
constexpr float MOTOR_V_NOM        = 12.0f;
constexpr float VBAT_CAP_EMA_ALPHA = 0.002f;

constexpr int ADC_OVERSAMPLE = 8;   // nb de lectures ADS1115 moyennées (lisse le résidu de bruit)
// L'ADS1115 en continu à 128 SPS ne produit une nouvelle valeur que toutes les ~8 ms : lire
// la tension à chaque tick de 2 ms gaspillerait ~4000 transactions I2C/s pour relire la même
// valeur. On lit à 20 Hz — largement assez pour la LVC (anti-rebond 500 ms) et le plafond PWM.
constexpr int VBAT_READ_TICKS = 25;   // 500 Hz / 25 = 20 Hz

// Convertisseur A/N externe ADS1115 (16 bits, I2C) — remplace l'ADC interne de l'ESP32.
// Sur le bus 0 (avec l'AS5600 gauche). Adresse réglée par ADDR ; 0x48 = ADDR→GND.
constexpr uint8_t ADS1115_ADDR = 0x48;

// Capteur d'angle AS5600 (I2C, 12 bits absolu = 4096 counts/tour). Cinématique (voir
// doc/reducteur.md) : boîte 16T→80T puis 32T→80T = 1:12,5 ; aimant sur la SORTIE DE BOÎTE,
// puis poulies 25T→32T (1,28:1) jusqu'à la roue → le capteur fait 1,28 tour par tour de roue
// ⇒ GEAR_RATIO = 1,28 (réduction totale moteur→roue : 12,5 × 1,28 = 16,0 PILE).
// Roue 10" = 0,254 m. Alim 3,3 V natif → AUCUN level-shift. Vitesse = dérivée de l'angle
// (Δcounts × CTRL_HZ) avec wrap 0↔4095 ; le SIGNE de Δ donne le sens. 500 Hz : sans ambiguïté.
constexpr float AS5600_CPR    = 4096.0f;  // counts par tour (12 bits)
constexpr float GEAR_RATIO    = 1.28f;    // tours capteur par tour de roue (sortie de boîte, poulies 32/25)
constexpr float WHEEL_DIAM_M  = 0.254f;   // roue 10"

constexpr int     I2C_FREQ_HZ       = 400000;  // Fast-mode (le capteur supporte jusqu'à 1 MHz)
constexpr uint8_t AS5600_ADDR       = 0x36;    // adresse I2C fixe (un seul capteur par bus)
constexpr uint8_t AS5600_REG_RAWANG = 0x0C;    // RAW ANGLE 12 bits (octets 0x0C MSB / 0x0D LSB)

constexpr int   VBAT_SAG_DEBOUNCE_MS = 500;
constexpr int   LVC_POWEROFF_MS      = 30000;  // coupure auto (powerOff) après 30 s sous le seuil

// ── Batterie : détection 12 V / 24 V au démarrage, seuils LVC codés en dur ──
// La tension doit rester STABLE (écart ≤ TOL) pendant 3 s, puis classement : une 12 V même
// en pleine charge reste ≤ ~14,8 V, une 24 V même déchargée reste ≥ ~21 V → le seuil 18 V
// tranche sans ambiguïté. On ne change JAMAIS de batterie système allumé : type figé jusqu'au
// redémarrage. Tant que non classée : pas de LVC (et le plafond PWM auto suit Vbat de toute
// façon). Seuils plomb par type — pas des paramètres web : liés à la chimie, pas au réglage.
constexpr int64_t VBAT_DETECT_STABLE_US = 3000000;   // 3 s de tension stable
constexpr float   VBAT_DETECT_TOL_V     = 0.5f;      // écart min-max toléré dans la fenêtre
constexpr float   VBAT_DETECT_24V_MIN   = 18.0f;     // moyenne stable ≥ 18 V → 24 V, sinon 12 V
constexpr float   VBAT12_WARN_V = 11.5f, VBAT12_CUT_V = 10.5f, VBAT12_RECOVER_V = 12.0f;
constexpr float   VBAT24_WARN_V = 23.0f, VBAT24_CUT_V = 21.0f, VBAT24_RECOVER_V = 24.0f;
// Pleine charge AU REPOS (haut de la jauge web ; le bas = seuil de coupure). L'échelle
// d'affichage est décidée côté firmware et envoyée dans le statut (batt_lo / batt_hi).
constexpr float   VBAT12_FULL_V = 13.0f;
constexpr float   VBAT24_FULL_V = 26.0f;
constexpr float EBRAKE_MIN_MPS       = 0.15f;  // en-dessous, on considère la roue arrêtée (frein PID)
constexpr float ENC_STUCK_PWM        = 0.10f;
constexpr int   ENC_STUCK_MS         = 1000;
// Sanité des encodeurs (câblage/montage) — un capteur qui MENT est pire qu'un capteur absent :
// tout défaut ci-dessous provoque l'ARRÊT TOTAL (désarmement + frein), verrouillé jusqu'au reboot.
// · INVERSÉ : consigne franche dans un sens, roue mesurée FRANCHEMENT dans l'autre pendant 400 ms
//   (évalué seulement hors freinage : le frein PID s'oppose à la rotation PAR DESIGN).
// · ABERRANT : |vitesse| physiquement impossible (kart ≈ 3,3 m/s max) pendant 200 ms.
constexpr float ENC_REV_PWM          = 0.25f;   // consigne « franche »
constexpr float ENC_REV_MPS         = 0.30f;    // vitesse « franche » opposée
constexpr int   ENC_REV_MS          = 400;
constexpr float ENC_MAX_SANE_MPS    = 8.0f;
constexpr int   ENC_MAD_MS          = 200;
// Lissage vitesse (moyenne exponentielle) : à 500 Hz le Δangle par tick est quantifié
// (~0,08 m/s par count avec GEAR_RATIO 1,28 / roue 10"). α ~0,25 → cte de temps ~4 ticks (8 ms).
constexpr float SPEED_EMA_ALPHA      = 0.25f;
constexpr int   BTN_DEBOUNCE_TICKS   = 3;

// Armement et retours haptiques (nommés — pas de nombres magiques dans le contrôleur).
constexpr float ARM_CENTER_MAX   = 0.08f;    // stick considéré « centré » pour armer
constexpr float PUSH_MIN         = 0.5f;     // stick considéré « poussé » (rumble si bloqué)
constexpr int64_t RUMBLE_BLOCK_INTERVAL_US = 800000;   // répétition du rumble « bloqué »
// Heartbeat manette : les manettes streament leurs rapports HID en continu (~10-20 ms).
// Lien « connecté » mais silence > 250 ms = communication perdue → désarmement + freinage
// IMMÉDIATS (le timeout de supervision Bluetooth, lui, prend plusieurs secondes).
constexpr int64_t PAD_HB_TIMEOUT_US  = 250000;
} // namespace hw

// ───────────────────────── Configuration (champs nommés, persistée) ─────────────────────────
// Tout est stocké en float (les entiers/bool aussi) → pointeur-vers-membre homogène.
struct KartConfig
{
    float speed_limit_ms;   // limite de vitesse VÉHICULE (m/s)
    float duty_cap_frac;
    float thr_deadzone;   // zone morte du manche (avance ET virage)
    float thr_ramp_per_s; // limiteur de pente de l'AVANCE (douceur, Δ/s)
    float vbat_div_ratio;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float vmax_kp;
    float vmax_ki;
    float vmax_kd;
    float turn_gain;     // part du différentiel à fond de manche X (0..1)
    float turn_limit_en; // 1 = anti-renversement actif (rampe vitesse→virage) ; 0 = désactivé (essais)
    float turn_full_ms;  // sous cette vitesse (m/s), virage ±100 % (pivot permis) — anti-renversement
    float turn_hi;       // limite de virage (0..1) atteinte à speed_limit_ms (rampe linéaire)
    float rev_limit;     // plafond d'avance en MARCHE ARRIÈRE (0..1) — recul bridé
    float turn_rate;     // pente max du virage (Δ/s) — adoucit les coups de manche brusques
    float vlim_enable;   // 1 = limiteur de vitesse PID actif ; 0 = désactivé (essais)
    float brk_pid_enable;// 1 = frein PID actif à l'arrêt ; 0 = frein dynamique seul (essais)
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
    const char*        desc;   // libellé court pour la page web
    const char*        cat;    // catégorie (regroupement visuel dans la page de config)
    const char*        help;   // description longue (infobulle au survol du champ)
    PType              type;
    float              min, def, max;
    float KartConfig::* field; // pointeur vers le champ correspondant
};

extern const ParamDesc PARAMS[];
extern const int       PARAM_COUNT;

// ───────────────────────── Télémétrie ─────────────────────────
enum class State : int { Lockout = 0, Calibrate = 1, Run = 2, Fault = 3 };
// Mode de freinage EFFECTIF (affiché en permanence sur la page web) :
// Dynamic = court-circuit des phases (état par défaut, désarmé, ou repli sans encodeurs) ;
// Active  = frein PID (consigne vitesse 0) — exige encodeurs présents ET brk_pid_enable=1.
enum class BrakeMode : int { None = 0, Dynamic = 1, Active = 2 };
enum class Fault : int { None = 0, EStop = 1, Lvc = 2, NotCalibrated = 3, Encoder = 4, EncoderDir = 5, EncoderMad = 6, EncoderAbsent = 7 };

// Bits du masque m_faults : TOUTES les conditions actives simultanément (m_fault ne retient
// que la plus prioritaire). Source unique côté firmware ; miroir de présentation côté web :
// FAULTS_DESC dans index.html (mêmes bits, textes seulement).
namespace fb
{
constexpr unsigned ESTOP     = 1u << 0;   // arrêt d'urgence manette (B)
constexpr unsigned LVC       = 1u << 1;   // batterie basse
constexpr unsigned NOCAL     = 1u << 2;   // manette non calibrée
constexpr unsigned ENC_STUCK = 1u << 3;   // roue bloquée (PWM sans rotation)
constexpr unsigned PAD_LOST  = 1u << 4;   // manette déconnectée
constexpr unsigned NO_VBAT   = 1u << 5;   // capteur de tension absent (info)
constexpr unsigned ENC_REV   = 1u << 6;   // encodeur/moteur câblé à l'envers
constexpr unsigned ENC_MAD   = 1u << 7;   // mesure de vitesse aberrante
constexpr unsigned ENC_L_ABS = 1u << 8;   // AS5600 gauche absent (I2C muet) — si use_encoders=1
constexpr unsigned ENC_R_ABS = 1u << 9;   // AS5600 droit absent — si use_encoders=1
constexpr unsigned PAD_STALE = 1u << 10;  // manette « connectée » mais muette > 250 ms (heartbeat)
} // namespace fb

struct KartStatus
{
    std::atomic<int>   m_state{static_cast<int>(State::Lockout)};
    std::atomic<int>   m_fault{static_cast<int>(Fault::None)};
    std::atomic<unsigned> m_faults{0};   // masque des conditions ACTIVES (page Défauts) — voir controller.cpp
    std::atomic<float> m_vbat{0.f};
    std::atomic<int>   m_batt_type{0};   // batterie détectée au démarrage : 0 = en cours, 12 ou 24 (V)
    std::atomic<float> m_speed_l{0.f};   // vitesse roue avant gauche SIGNÉE (AS5600 #1, m/s)
    std::atomic<float> m_speed_r{0.f};   // vitesse roue avant droite SIGNÉE (AS5600 #2, m/s)
    std::atomic<float> m_speed_ms{0.f};  // vitesse VÉHICULE signée (m/s) = (vG+vD)/2 — pivot sur place → 0
    std::atomic<float> m_fwd{0.f};       // consigne d'avance après mix/limites [-1..1]
    std::atomic<float> m_turn{0.f};      // consigne de virage après anti-renversement [-1..1]
    std::atomic<bool>  m_btn_start{false};
    std::atomic<float> m_out_l{0.f};     // PWM moteur gauche [-1..1]
    std::atomic<float> m_out_r{0.f};     // PWM moteur droite [-1..1]
    std::atomic<int>   m_brake_mode{static_cast<int>(BrakeMode::Dynamic)};   // BrakeMode effectif
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
