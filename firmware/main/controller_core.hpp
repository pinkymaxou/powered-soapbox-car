// controller_core.hpp — Cœur du contrôle du kart : classe ABSTRAITE, 100 % pure (aucune
// dépendance ESP-IDF, compilable sur l'hôte). Toutes les entrées/sorties passent par des
// callbacks virtuels io* : le firmware les branche sur le matériel (EspController dans
// controller.cpp), les tests sur un simulateur physique (SimController dans test_host/sim).
// La logique de tick() est LA référence — identique sur cible et en simulation.
#pragma once

#include <cstdint>

#include "control_math.hpp"
#include "control_types.hpp"
#include "pid.hpp"

// Sous-ensemble de l'état manette utile à la LOGIQUE (les champs d'affichage — gâchettes,
// stick droit, masque de boutons — restent l'affaire du wrapper ESP).
struct CtrlPad
{
    float x = 0.f;           // virage CALIBRÉ [-1..1] (+ = droite)
    float y = 0.f;           // avance CALIBRÉE [-1..1] (+ = avant)
    float rx = 0.f;          // virage BRUT (détection « pousse mais bloqué »)
    float ry = 0.f;          // avance BRUTE
    bool  connected = false;
    bool  estop = false;     // bouton d'arrêt manette (B)
    bool  start = false;     // bouton START/Options (armement)
};

// Télémétrie produite par chaque tick — publiée par le wrapper (g_status côté ESP,
// asserts/traces côté simulation).
struct CtrlTelemetry
{
    State     state = State::Lockout;
    Fault     fault = Fault::None;
    unsigned  faults = 0;        // masque des conditions actives (bits fb::)
    float     vbat = 0.f;
    int       batt_type = 0;     // 0 = en détection, 12 ou 24
    float     speed_l = 0.f;     // vitesse roue gauche SIGNÉE (m/s)
    float     speed_r = 0.f;
    float     speed_ms = 0.f;    // vitesse VÉHICULE signée (m/s), 0 en pivot
    float     fwd = 0.f;         // consigne d'avance après limites [-1..1]
    float     turn = 0.f;        // consigne de virage après anti-renversement [-1..1]
    float     out_l = 0.f;       // PWM moteur gauche [-1..1]
    float     out_r = 0.f;
    BrakeMode brake_mode = BrakeMode::Dynamic;
    bool      armed = false;
    bool      btn_start = false; // START physique OU manette (affichage)
};

class ControllerBase
{
public:
    virtual ~ControllerBase() = default;

    // Un pas de contrôle (période hw::CTRL_DT_S). Lit les entrées via les callbacks GET,
    // écrit moteurs/rumble via les callbacks SET, remplit telemetry().
    void tick(const KartConfig& cfg);

    const CtrlTelemetry& telemetry() const { return m_tel; }

protected:
    // ── Callbacks GET (entrées) ──
    virtual int64_t ioNowUs() = 0;             // horloge monotone (µs)
    virtual CtrlPad ioPad() = 0;               // dernier état manette
    virtual int64_t ioPadLastReportUs() = 0;   // date du dernier rapport HID (heartbeat)
    virtual bool    ioPadCalibrated() = 0;
    virtual bool    ioBtnStart() = 0;          // bouton START physique (anti-rebond fait)
    virtual float   ioVbatRaw() = 0;           // tension à la broche ADC (< 0 si capteur absent)
    virtual int     ioEncDeltaL() = 0;         // Δcounts AS5600 depuis le dernier appel
    virtual int     ioEncDeltaR() = 0;
    virtual bool    ioEncPresentL() = 0;       // dernière lecture I2C réussie
    virtual bool    ioEncPresentR() = 0;

    // ── Callbacks SET (sorties) ──
    virtual void ioMotorsSet(float l, float r, uint32_t cap) = 0;   // PWM signé + plafond duty
    virtual void ioMotorsBrake() = 0;                               // court-circuit des phases
    virtual void ioRumble(uint8_t strong, uint8_t weak, uint16_t dur_ms) = 0;
    virtual void ioPowerOff() = 0;                                  // coupure alim (LVC prolongée)
    virtual void ioFlushPendingConfig() {}     // front armé→désarmé (persistance NVS différée)

private:
    // Méthodes internes (ex-fonctions libres de controller.cpp)
    void  updateLVC(float vbat, int64_t now);
    void  updateEncStuck(int64_t now, float cmd, float speed_ms);
    void  updateEncSanity(int64_t now, bool braking, float out_l, float out_r, float sl, float sr);
    float brakeWheel(Pid& pid, float speed_ms, const KartConfig& cfg, float dt);

    CtrlTelemetry m_tel;

    // ── État de la boucle (ex-namespace anonyme de controller.cpp) ──
    Pid     m_brake_l;             // frein roue gauche (consigne vitesse 0, sortie signée)
    Pid     m_brake_r;
    Pid     m_speed_pid;           // plafond de vitesse global (sortie = fraction de PWM)
    bool    m_armed = false;
    bool    m_lvc_tripped = false;
    int64_t m_sag_start_us = 0;
    int64_t m_lvc_since_us = 0;
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
    float   m_sl_ema = 0.f;
    float   m_sr_ema = 0.f;
    int     m_vbat_tick = 0;           // rate-limit de lecture Vbat (hw::VBAT_READ_TICKS)
    float   m_vraw = -1.f;             // dernière lecture brute (< 0 = capteur absent)

    // Retours haptiques : fronts + anti-spam
    bool    m_armed_prev = false;
    bool    m_estop_prev = false;
    bool    m_hardfault_prev = false;
    int64_t m_rumble_block_us = 0;
};
