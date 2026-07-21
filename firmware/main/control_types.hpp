// control_types.hpp — Types et constantes PURS du contrôle (compilables sur l'hôte).
// Extrait de config.hpp pour que la logique de contrôle (controller_core) et le simulateur
// (test_host/sim) partagent la MÊME source de vérité : KartConfig, énumérations d'état/défaut
// et constantes hw:: — sans aucune dépendance ESP-IDF.
#pragma once

#include <cstdint>

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

constexpr int PWM_FREQ_HZ = 18000;
constexpr int PWM_MAX     = 4095;   // 12 bits (la résolution LEDC vit dans hardware.cpp)

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
constexpr float ENC_REV_DECAY_MPS   = 0.15f;    // |v| qui décroît d'autant = décélération, pas une inversion
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
    float speed_limit_ms;   // limite de vitesse VÉHICULE en marche AVANT (m/s)
    float rev_speed_ms;     // limite de vitesse en marche ARRIÈRE (m/s) — distincte
    float duty_cap_frac;
    float thr_deadzone;   // zone morte du manche (avance ET virage)
    float thr_ramp_per_s; // limiteur de pente de l'AVANCE (douceur, Δ/s)
    float vbat_div_ratio;
    float brk_kp;
    float brk_ki;
    float brk_kd;
    float vlim_kp;
    float vlim_ki;
    float vlim_kd;
    float turn_gain;     // part du différentiel à fond de manche X (0..1)
    float turn_limit_en; // 1 = anti-renversement actif (rampe vitesse→virage) ; 0 = désactivé (essais)
    float turn_full_ms;  // sous cette vitesse (m/s), virage ±100 % (pivot permis) — anti-renversement
    float turn_hi;       // limite de virage (0..1) atteinte à speed_limit_ms (rampe linéaire)
    float turn_rate;     // pente max du virage (Δ/s) — adoucit les coups de manche brusques
    float vlim_enable;   // 1 = limiteur de vitesse PID actif ; 0 = désactivé (essais)
    float brk_pid_enable;// 1 = frein PID actif à l'arrêt ; 0 = frein dynamique seul (essais)
    float use_encoders;  // 1 = asservissement vitesse/frein/défaut via AS5600 ; 0 = ignore les encodeurs
    float allow_reverse;
    float arm_hold_ms;
    float disarm_s;
    float led_count;
    float led_brightness;

    // Conversion des ticks d'encodeur — DONNÉE AU CONTRÔLEUR PAR CONFIGURATION (l'hôte peut
    // la changer : capteur/réduction/roue différents). Hors PARAMS : ni NVS ni page web —
    // c'est du matériel, pas du réglage. Défauts (setDefaults) : AS5600 + réducteur réels.
    float enc_mps_per_cps;   // (m/s roue) par (count/s) — soit : mètres par count
    float enc_rpm_per_cps;   // (tr/min roue) par (count/s)

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

// Agrégats : BLOCKING interdit la conduite (désarmement + State::Fault) ;
// HARD mérite le rumble fort (tout défaut bloquant sauf la calibration manquante).
constexpr unsigned BLOCKING = LVC | NOCAL | ENC_STUCK | ENC_REV | ENC_MAD | ENC_L_ABS | ENC_R_ABS;
constexpr unsigned HARD     = BLOCKING & ~NOCAL;
} // namespace fb

// Défaut PRIORITAIRE dérivé du bitset — le cœur ne publie QUE le masque ; l'enum Fault ne
// sert qu'à l'affichage (champ « fault » du protobuf) et aux asserts des tests. Même ordre
// de priorité que l'ancienne cascade du contrôleur.
inline Fault primaryFault(unsigned faults)
{
    if (faults & fb::LVC)                         return Fault::Lvc;
    if (faults & (fb::ENC_L_ABS | fb::ENC_R_ABS)) return Fault::EncoderAbsent;
    if (faults & fb::ENC_MAD)                     return Fault::EncoderMad;
    if (faults & fb::ENC_REV)                     return Fault::EncoderDir;
    if (faults & fb::ENC_STUCK)                   return Fault::Encoder;
    if (faults & fb::NOCAL)                       return Fault::NotCalibrated;
    return Fault::None;
}

