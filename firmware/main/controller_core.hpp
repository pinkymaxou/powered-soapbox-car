// controller_core.hpp — Cœur du contrôle du kart : une classe PURE (aucune dépendance
// ESP-IDF, compilable sur l'hôte) qui contient TOUTE la logique métier pour opérer le kart.
// Un hôte (matériel réel ou simulation) n'a qu'à :
//   1. remplir les deux callbacks (setCallbacks) : updateSensors → lecture encodeurs/batterie,
//      updateOutputs → application des PWM/frein et des événements (rumble, coupure…) ;
//   2. pousser les entrées quand elles changent : setPad(), setStartButton(), setConfig() ;
//   3. appeler tick(now_us) à la période hw::CTRL_DT_S.
// L'API publique est THREAD-SAFE D'OFFICE (mutex interne) : la config peut arriver d'une
// tâche web pendant que la boucle de contrôle tick() et qu'un lecteur copie telemetry().
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "control_math.hpp"
#include "control_types.hpp"
#include "pid.hpp"

// Lecture des CAPTEURS (encodeurs + batterie) : données ET erreurs de lecture — c'est le
// retour du callback updateSensors de l'hôte (bus I2C réel ou modèle physique).
struct SensorReadings
{
    int   enc_delta_l = 0;    // Δcounts AS5600 depuis le dernier tick
    int   enc_delta_r = 0;
    bool  enc_ok_l = false;   // lecture I2C réussie (false = capteur absent/muet)
    bool  enc_ok_r = false;
    float vbat_v = -1.f;      // tension BATTERIE en VOLTS — la conversion broche ADC →
                              // batterie (ratio du diviseur) se fait CHEZ L'HÔTE
    bool  vbat_ok = false;    // false = ADS1115 absent ou lecture en erreur
};

// État MANETTE poussé par l'hôte (setPad) : pourcentages [-1..1] des sticks et boutons.
// C'est le DERNIER état connu — tick() s'en sert tel quel à chaque pas.
struct PadInputs
{
    float x = 0.f;                   // consigne de virage CALIBRÉE (+ = droite)
    float y = 0.f;                   // consigne d'avance CALIBRÉE (+ = avant, − = recul)
    float rx = 0.f;                  // stick BRUT (détection « pousse mais bloqué »)
    float ry = 0.f;
    bool  connected = false;
    bool  calibrated = false;
    bool  estop = false;             // bouton d'arrêt manette (B)
    bool  start = false;             // bouton START/Options manette
    int64_t last_report_us = 0;      // date du dernier rapport HID (heartbeat)
};

// Entrées ASSEMBLÉES d'un pas — construites par tick() depuis l'état poussé + le callback
// capteurs, consommées par la logique métier (step).
struct CtrlInputs
{
    int64_t now_us = 0;              // horloge monotone (µs)
    PadInputs pad;                   // manette (voir setPad)
    bool  btn_start_hw = false;      // bouton START physique (voir setStartButton)
    SensorReadings sensors;          // encodeurs + tension batterie (voir setCallbacks)
};

// ── Sortie d'un tick : la COMMANDE MOTEUR, rien d'autre ──
// Le cœur calcule une sortie moteur en fonction des entrées. Rumble, coupure
// d'alimentation, persistance de config : décisions d'HÔTE dérivées de la télémétrie
// (voir advisors.hpp et les hôtes EspController/SimController).
struct CtrlOutputs
{
    // SOIT le court-circuit des phases (dyn_brake), SOIT les PWM signés plafonnés.
    bool     dyn_brake = true;       // état par défaut : freinage (jamais en roue libre)
    float    out_l = 0.f;            // PWM roue gauche [-1..1]
    float    out_r = 0.f;
    uint32_t cap = 0;                // plafond duty (0..hw::PWM_MAX)
};

// Télémétrie du tick — copie cohérente via telemetry() (publiée dans g_status côté ESP,
// inspectée par les asserts en simulation).
struct CtrlTelemetry
{
    State     state = State::Lockout;
    unsigned  faults = 0;            // BITSET des erreurs/conditions actives (bits fb::) —
                                     // seule représentation des défauts ; le défaut
                                     // prioritaire d'affichage se dérive via primaryFault()
    float     vbat = 0.f;
    int       batt_type = 0;         // 0 = en détection, 12 ou 24
    float     speed_l = 0.f;         // vitesse roue gauche SIGNÉE (m/s) — ratio enc_mps_per_cps
    float     speed_r = 0.f;
    float     rpm_l = 0.f;           // roue gauche SIGNÉE (tr/min) — ratio enc_rpm_per_cps
    float     rpm_r = 0.f;
    float     speed_ms = 0.f;        // vitesse VÉHICULE signée (m/s), 0 en pivot
    float     fwd = 0.f;             // consigne d'avance après limites [-1..1]
    float     turn = 0.f;            // consigne de virage après anti-renversement [-1..1]
    float     out_l = 0.f;
    float     out_r = 0.f;
    BrakeMode brake_mode = BrakeMode::Dynamic;
    bool      armed = false;
    bool      btn_start = false;     // START physique OU manette (affichage)
};

class KartController
{
public:
    using UpdateSensorsFn = std::function<SensorReadings()>;
    using UpdateOutputsFn = std::function<void(const CtrlOutputs&)>;

    KartController() { m_cfg.setDefaults(); }

    // Câblage vers le monde de l'hôte (une fois, avant la boucle) : updateSensors est
    // APPELÉ par tick() pour lire les capteurs ; updateOutputs REÇOIT les sorties du pas.
    // Sans câblage : capteurs « absents » (SensorReadings{}), sorties perdues.
    void setCallbacks(UpdateSensorsFn updateSensors, UpdateOutputsFn updateOutputs);

    // ── Entrées (thread-safe, à pousser quand l'info change — le tick suivant s'en sert) ──
    void setConfig(const KartConfig& cfg);
    void setPad(const PadInputs& pad);
    void setStartButton(bool held);   // bouton START physique (anti-rebond fait par l'hôte)

    // Un pas de contrôle (période hw::CTRL_DT_S) : lit les capteurs (callback), déroule
    // TOUTE la logique métier sous verrou, applique les sorties (callback) — et les retourne.
    CtrlOutputs tick(int64_t now_us);

    // ── Lectures (thread-safe : copies cohérentes sous mutex) ──
    KartConfig    config() const;
    CtrlTelemetry telemetry() const;

private:
    CtrlOutputs step(const CtrlInputs& in);   // la logique métier d'un pas (sous verrou)

    void  updateLVC(float vbat, int64_t now);
    void  updateEncStuck(int64_t now, float cmd, float speed_ms);
    void  updateEncSanity(int64_t now, bool braking, float out_l, float out_r, float sl, float sr);
    float brakeWheel(Pid& pid, float speed_ms, const KartConfig& cfg, float dt);

    mutable std::mutex m_mtx;   // thread-safety d'office de toute l'API publique

    UpdateSensorsFn m_update_sensors;
    UpdateOutputsFn m_update_outputs;
    KartConfig m_cfg;                  // configurations courantes (voir setConfig)
    PadInputs  m_pad;                  // dernier état manette poussé (voir setPad)
    bool       m_btn_start_hw = false; // dernier état du bouton START physique

    CtrlTelemetry m_tel;

    // ── État de la boucle ──
    Pid     m_brake_l;             // frein roue gauche (consigne vitesse 0, sortie signée)
    Pid     m_brake_r;
    Pid     m_speed_pid;           // plafond de vitesse global (sortie = fraction de PWM)
    bool    m_armed = false;
    bool    m_lvc_tripped = false;
    int64_t m_sag_start_us = 0;
    int64_t m_hold_start_us = 0;   // début d'appui START
    int64_t m_last_act_us = 0;     // dernière activité manche
    bool    m_start_latch = false; // appui START déjà traité
    int64_t m_stuck_us = 0;
    bool    m_enc_fault = false;
    ctl::RevDetect m_rev_l, m_rev_r;   // « sens inversé » par roue
    int64_t m_mad_us = 0;              // début de mesure aberrante persistante
    bool    m_enc_rev_fault = false;   // encodeur/moteur câblé à l'envers (verrouillé)
    bool    m_enc_mad_fault = false;   // mesure sans sens physique (verrouillé)
    float   m_fwd_cmd = 0.f;           // consigne avance APRÈS limiteur de pente
    float   m_turn_cmd = 0.f;
    float   m_vbat_ema = 0.f;          // Vbat lissée (τ ≈ 1 s) pour le plafond PWM — 0 = inconnue
    ctl::BattDetect m_batt_det;        // type 12/24 V classé au démarrage (tension stable 3 s)
    float   m_cps_l = 0.f;             // taux de counts encodeur LISSÉ (EMA, counts/s)
    float   m_cps_r = 0.f;
    int     m_vbat_tick = 0;           // rate-limit de lecture Vbat (hw::VBAT_READ_TICKS)
    float   m_vraw = -1.f;             // dernière lecture brute (< 0 = capteur absent)
};
