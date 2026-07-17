// controller.cpp — Contrôle DIFFÉRENTIEL (variante expérimentale).
// 2 roues avant motrices indépendantes + 1 roulette arrière. Pilotage manette (namespace input).
// Mix arcade : y = avance, x = virage → gauche = avance + virage, droite = avance − virage.
// Sécurités : manette déconnectée / e-stop manette / non armé / LVC → FREINAGE (PID vitesse → 0).
// Anti-renversement : le virage autorisé décroît avec la vitesse (borne l'accélération latérale).
// ESP-IDF 6.1 / C++.
#include "controller.hpp"

#include <algorithm>
#include <cmath>

#include "config.hpp"
#include "control_math.hpp"
#include "hardware.hpp"
#include "input.hpp"
#include "pid.hpp"
#include "rtos.hpp"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
using ctl::clampf;
using ctl::deadzone;
using ctl::mixArcade;
using ctl::slew;
using ctl::turnLimit;

constexpr float PI_F = 3.14159265358979f;

// Δcounts AS5600 (12 bits, 4096/tour) sur un tick → vitesse roue en m/s, SIGNÉE (sens inclus).
// Tient compte du réducteur (GEAR_RATIO = tours capteur par tour de roue) et de la roue 10".
float countsToMps(int delta)
{
    const float sensor_rps = (static_cast<float>(delta) / hw::AS5600_CPR) * hw::CTRL_HZ;
    const float wheel_rps = sensor_rps / hw::GEAR_RATIO;
    return wheel_rps * PI_F * hw::WHEEL_DIAM_M;
}

// (deadzone / slew / clampf / mixArcade : voir control_math.hpp — testés sur l'hôte.)

// ── État interne de la boucle ──
Pid     m_brake_l;       // frein roue gauche (consigne vitesse 0, sortie signée)
Pid     m_brake_r;       // frein roue droite
Pid     m_speed_pid;     // plafond de vitesse global (sortie = fraction de PWM autorisée)
bool    m_armed = false;
bool    m_lvc_tripped = false;
int64_t m_sag_start_us = 0;
int64_t m_lvc_since_us = 0;
int64_t m_hold_start_us = 0;   // début d'appui START
int64_t m_last_act_us = 0;     // dernière activité manche
bool    m_start_latch = false; // appui START déjà traité
int64_t m_stuck_us = 0;
bool    m_enc_fault = false;
int64_t m_rev_l_us = 0, m_rev_r_us = 0;   // débuts de « sens opposé » persistant (G/D)
int64_t m_mad_us = 0;                      // début de mesure aberrante persistante
bool    m_enc_rev_fault = false;           // encodeur/moteur câblé À L'ENVERS (verrouillé)
bool    m_enc_mad_fault = false;           // mesure sans aucun sens physique (verrouillé)
float   m_fwd_cmd = 0.f;   // consigne avance APRÈS limiteur de pente (état conservé entre ticks)
float   m_turn_cmd = 0.f;  // consigne virage APRÈS limiteur de pente
float   m_vbat_ema = 0.f;  // Vbat lissée LENTEMENT (τ ≈ 1 s) pour le plafond PWM auto — 0 = inconnue
ctl::BattDetect m_batt_det; // type de batterie (12/24 V) classé au démarrage sur tension stable 3 s
float   m_sl_ema = 0.f;    // vitesse roue G lissée (EMA)
float   m_sr_ema = 0.f;    // vitesse roue D lissée (EMA)

// Retours haptiques : détection de fronts + anti-spam.
bool    m_armed_prev = false;
bool    m_estop_prev = false;
bool    m_hardfault_prev = false;
int64_t m_rumble_block_us = 0;   // dernier buzz « tentative de bouger non armé »

void setState(State s, Fault f)
{
    g_status.m_state.store(static_cast<int>(s));
    g_status.m_fault.store(static_cast<int>(f));
}

void updateLVC(float vbat)
{
    // Seuils codés en dur selon la batterie DÉTECTÉE au démarrage (12 V ou 24 V, plomb).
    // Tant que la tension n'a pas été stable 3 s (type inconnu) : pas de LVC — le kart
    // démarre désarmé de toute façon, et on ne change jamais de batterie système allumé.
    const int bt = m_batt_det.volts;
    if (0 == bt)
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        m_lvc_since_us = 0;
        return;
    }
    const float cut_v     = (24 == bt) ? hw::VBAT24_CUT_V     : hw::VBAT12_CUT_V;
    const float recover_v = (24 == bt) ? hw::VBAT24_RECOVER_V : hw::VBAT12_RECOVER_V;

    const int64_t now = esp_timer_get_time();
    if (!m_lvc_tripped)
    {
        if (vbat < cut_v)
        {
            if (0 == m_sag_start_us) m_sag_start_us = now;
            if ((now - m_sag_start_us) > static_cast<int64_t>(hw::VBAT_SAG_DEBOUNCE_MS) * 1000)
            {
                m_lvc_since_us = now;
                m_lvc_tripped = true;
            }
        }
        else
        {
            m_sag_start_us = 0;
        }
    }
    else if (vbat > recover_v)
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        m_lvc_since_us = 0;
    }
}

// Freine une roue : PID vitesse → 0 (sortie signée, peut inverser). Vitesse en m/s.
float brakeWheel(Pid& pid, float speed_ms, const KartConfig& cfg, float dt)
{
    if (fabsf(speed_ms) <= hw::EBRAKE_MIN_MPS)
    {
        pid.reset();
        return 0.f;
    }
    return pid.update(0.f, speed_ms, dt, cfg.pid_kp, cfg.pid_ki, cfg.pid_kd, -1.f, 1.f);
}

void updateEncStuck(int64_t now, float cmd, float speed_ms)
{
    if (fabsf(cmd) > hw::ENC_STUCK_PWM && fabsf(speed_ms) <= 0.05f)
    {
        if (0 == m_stuck_us) m_stuck_us = now;
        else if ((now - m_stuck_us) > static_cast<int64_t>(hw::ENC_STUCK_MS) * 1000) m_enc_fault = true;
    }
    else
    {
        m_stuck_us = 0;
    }
}

// Sanité des encodeurs : sens inversé (roue mesurée à l'opposé d'une consigne franche —
// câblage capteur OU moteur à l'envers) et mesure aberrante (vitesse impossible). Un capteur
// qui MENT rend le frein PID et le limiteur DANGEREUX (ils pousseraient au lieu de retenir) →
// ARRÊT TOTAL, verrouillé jusqu'au redémarrage. « braking » exclut le frein PID : il s'oppose
// à la rotation par design et déclencherait le test de sens à tort.
void updateEncSanity(int64_t now, bool braking, float out_l, float out_r, float sl, float sr)
{
    auto reversed = [now](float out, float v, int64_t& t0) -> bool {
        if (fabsf(out) > hw::ENC_REV_PWM && fabsf(v) > hw::ENC_REV_MPS && out * v < 0.f)
        {
            if (0 == t0) t0 = now;
            return (now - t0) > static_cast<int64_t>(hw::ENC_REV_MS) * 1000;
        }
        t0 = 0;
        return false;
    };
    if (!braking)
    {
        if (reversed(out_l, sl, m_rev_l_us) || reversed(out_r, sr, m_rev_r_us)) m_enc_rev_fault = true;
    }
    else
    {
        m_rev_l_us = m_rev_r_us = 0;
    }

    if (fabsf(sl) > hw::ENC_MAX_SANE_MPS || fabsf(sr) > hw::ENC_MAX_SANE_MPS)
    {
        if (0 == m_mad_us) m_mad_us = now;
        else if ((now - m_mad_us) > static_cast<int64_t>(hw::ENC_MAD_MS) * 1000) m_enc_mad_fault = true;
    }
    else
    {
        m_mad_us = 0;
    }
}

void publish(float sl, float sr, float v_veh, float vbat, const input::State& in)
{
    g_status.m_vbat.store(vbat);
    g_status.m_batt_type.store(m_batt_det.volts);
    g_status.m_speed_l.store(sl);
    g_status.m_speed_r.store(sr);
    g_status.m_speed_ms.store(v_veh);   // vitesse véhicule signée (m/s), 0 en pivot
    g_status.m_pad_conn.store(in.connected);
    g_status.m_pad_batt.store(input::battery());
    g_status.m_btn_start.store(board::btnStart() || in.start);   // START physique OU manette
    g_status.m_estop.store(in.estop);
    g_status.m_pad_x.store(in.rx);          // position physique du stick (cercle)
    g_status.m_pad_y.store(in.ry);
    g_status.m_pad_cx.store(in.x);          // consigne compensée cercle→carré
    g_status.m_pad_cy.store(in.y);
    g_status.m_pad_zl.store(in.zl);
    g_status.m_pad_zr.store(in.zr);
    g_status.m_pad_rx2.store(in.rx2);
    g_status.m_pad_ry2.store(in.ry2);
    g_status.m_pad_btns.store(in.buttons);
}

void tick()
{
    const KartConfig cfg = configSnapshot();
    const int64_t now = esp_timer_get_time();

    const bool use_enc = (cfg.use_encoders != 0.f);   // 0 = ignore les AS5600 (banc sans encodeurs)

    board::pollButtons();
    const input::State in = input::get();
    // Heartbeat : « connectée » mais aucun rapport HID depuis 250 ms → traitée comme
    // DÉCONNECTÉE (désarmement + freinage immédiats, sans attendre le timeout Bluetooth).
    const bool pad_stale = in.connected &&
                           ((now - input::lastReportUs()) > hw::PAD_HB_TIMEOUT_US);
    // Tension lue à 20 Hz seulement (hw::VBAT_READ_TICKS) : l'ADS1115 à 128 SPS ne produit
    // rien de neuf plus vite, et ça évite ~4000 transactions I2C/s dans la boucle 500 Hz.
    static int   vbat_tick = 0;
    static float vraw = -1.f;   // < 0 si capteur (ADS1115) absent
    if (0 == (vbat_tick++ % hw::VBAT_READ_TICKS))
    {
        vraw = board::vbatVolts(hw::ADC_OVERSAMPLE);
    }
    const bool  vbat_valid = (vraw > 0.05f);
    const float vbat = vbat_valid ? vraw * cfg.vbat_div_ratio : 0.f;
    const float sl_raw = use_enc ? countsToMps(board::encLeftDelta())  : 0.f;
    const float sr_raw = use_enc ? countsToMps(board::encRightDelta()) : 0.f;
    // Lissage EMA : atténue la quantification du Δangle par tick à basse vitesse.
    m_sl_ema += hw::SPEED_EMA_ALPHA * (sl_raw - m_sl_ema);
    m_sr_ema += hw::SPEED_EMA_ALPHA * (sr_raw - m_sr_ema);
    const float sl = m_sl_ema, sr = m_sr_ema;   // vitesses roues SIGNÉES (m/s)
    // Vitesse VÉHICULE (m/s) = moyenne signée des deux roues : deux roues égales en sens
    // inverse (pivot sur place) → 0 m/s. C'est elle qui sert aux ajustements de conduite.
    const float v_veh = 0.5f * (sl + sr);
    if (!use_enc) m_enc_fault = false;   // pas d'encodeurs → pas de défaut « capteur bloqué »
    if (!use_enc) { m_enc_rev_fault = false; m_enc_mad_fault = false; m_rev_l_us = m_rev_r_us = m_mad_us = 0; }

    // Capteur de tension absent → tension inconnue : on NE déclenche PAS la LVC (le BMS du
    // pack assure la protection). Permet aussi de tester au banc sans l'ADS1115 câblé.
    if (vbat_valid)
    {
        m_batt_det.update(vbat, now, hw::VBAT_DETECT_STABLE_US,
                          hw::VBAT_DETECT_TOL_V, hw::VBAT_DETECT_24V_MIN);
        updateLVC(vbat);
        // Lissage LENT pour le plafond PWM auto : l'affaissement sous charge ne doit pas
        // faire osciller le duty (sag → Vbat baisse → duty remonte → plus de sag…).
        if (m_vbat_ema <= 0.f) m_vbat_ema = vbat;
        else m_vbat_ema += hw::VBAT_CAP_EMA_ALPHA * (vbat - m_vbat_ema);
    }
    else
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        m_lvc_since_us = 0;
        m_vbat_ema = 0.f;   // tension inconnue → pas de plafond automatique
    }
    publish(sl, sr, v_veh, vbat, in);

    // Plafond PWM : AUTOMATIQUE (12 V nominaux / Vbat mesurée : 12 V → ~100 %, 24 V → ~50 %)
    // ET manuel (duty_cap, page web) — le plus restrictif gagne. Sans ADS1115 : manuel seul.
    const float duty_max = std::min(cfg.duty_cap_frac, ctl::dutyCapVolts(m_vbat_ema, hw::MOTOR_V_NOM));
    const uint32_t cap = static_cast<uint32_t>(hw::PWM_MAX * clampf(duty_max, 0.f, 1.f));

    // ── Défauts / conditions de non-conduite ──
    // Encodeurs ABSENTS (I2C muet) : avec use_encoders=1 c'est bloquant — frein PID et
    // limiteur croiraient la roue arrêtée. Avec use_encoders=0 (banc) : simplement ignorés.
    const bool enc_l_abs = use_enc && !board::encLeftPresent();
    const bool enc_r_abs = use_enc && !board::encRightPresent();

    Fault fault = Fault::None;
    if (m_lvc_tripped)                              fault = Fault::Lvc;
    else if (enc_l_abs || enc_r_abs)                fault = Fault::EncoderAbsent;  // capteur muet
    else if (m_enc_mad_fault)                       fault = Fault::EncoderMad;     // mesure impossible
    else if (m_enc_rev_fault)                       fault = Fault::EncoderDir;     // sens inversé
    else if (m_enc_fault)                           fault = Fault::Encoder;        // roue bloquée
    else if (in.connected && !input::calibrated())  fault = Fault::NotCalibrated;  // manette non calibrée

    // Masque de TOUTES les conditions actives (le champ « fault » ne retient que la plus
    // prioritaire) — consommé par la page web « Défauts ». Bits : voir FAULTS_DESC (index.html).
    unsigned fmask = 0;
    if (in.estop)                                fmask |= fb::ESTOP;
    if (m_lvc_tripped)                           fmask |= fb::LVC;
    if (in.connected && !input::calibrated())    fmask |= fb::NOCAL;
    if (m_enc_fault)                             fmask |= fb::ENC_STUCK;
    if (!in.connected)                           fmask |= fb::PAD_LOST;
    if (pad_stale)                               fmask |= fb::PAD_STALE;
    if (!vbat_valid)                             fmask |= fb::NO_VBAT;
    if (m_enc_rev_fault)                         fmask |= fb::ENC_REV;
    if (m_enc_mad_fault)                         fmask |= fb::ENC_MAD;
    if (enc_l_abs)                               fmask |= fb::ENC_L_ABS;
    if (enc_r_abs)                               fmask |= fb::ENC_R_ABS;
    g_status.m_faults.store(fmask);

    // Manette absente / e-stop manette / DÉFAUT → on désarme et on freine (sécurité absolue).
    // Un défaut force le désarmement : il faudra réarmer (START maintenu) une fois résolu.
    if (!in.connected || pad_stale || in.estop || (Fault::None != fault)) m_armed = false;

    const bool can_drive = m_armed && in.connected && !pad_stale && !in.estop && (Fault::None == fault);

    // ── Armement par appui maintenu sur START (anti-démarrage : manche centré + manette connectée) ──
    // START = bouton physique OU bouton START/Options de la manette (même fonction).
    const bool start_held = board::btnStart() || in.start;
    const bool centered = (fabsf(in.x) < hw::ARM_CENTER_MAX) && (fabsf(in.y) < hw::ARM_CENTER_MAX);
    if (start_held)
    {
        if (0 == m_hold_start_us) m_hold_start_us = now;
        else if (!m_start_latch && (now - m_hold_start_us) > static_cast<int64_t>(cfg.arm_hold_ms) * 1000)
        {
            m_start_latch = true;
            if (!m_armed && in.connected && centered) { m_armed = true; m_last_act_us = now; }
            else                                       { m_armed = false; }
        }
    }
    else
    {
        m_hold_start_us = 0;
        m_start_latch = false;
    }

    // ── Retours haptiques manette (sur fronts) ──
    // Défaut « dur » (rumble fort) : LVC ou N'IMPORTE QUEL défaut encodeur (bloqué/inversé/
    // aberrant/absent) — tous forcent l'arrêt et méritent un retour haptique appuyé.
    const bool hard_fault = (Fault::Lvc == fault) || (Fault::Encoder == fault) ||
                            (Fault::EncoderDir == fault) || (Fault::EncoderMad == fault) ||
                            (Fault::EncoderAbsent == fault);
    if (m_armed && !m_armed_prev)                               // vient d'être armé → doux
        input::rumble(90, 160, 220);
    if ((hard_fault && !m_hardfault_prev) || (in.estop && !m_estop_prev))  // erreur soudaine / e-stop → fort
        input::rumble(255, 255, 450);
    const bool pushing = (fabsf(in.rx) > hw::PUSH_MIN) || (fabsf(in.ry) > hw::PUSH_MIN);
    if (pushing && !can_drive && (now - m_rumble_block_us) > hw::RUMBLE_BLOCK_INTERVAL_US)   // bouge mais bloqué → fort (répété)
    {
        input::rumble(220, 220, 250);
        m_rumble_block_us = now;
    }
    m_armed_prev = m_armed;
    m_estop_prev = in.estop;
    m_hardfault_prev = hard_fault;

    float out_l = 0.f, out_r = 0.f, fwd = 0.f, turn = 0.f;
    bool braking = false;
    bool dyn_brake = false;   // true → court-circuit moteur (freinage dynamique passif)

    if (!can_drive)
    {
        // NON ARMÉ / défaut / e-stop / manette déconnectée → FREINAGE DYNAMIQUE (court-circuit
        // moteur). État par défaut au repos ; ne nécessite ni encodeurs ni asservissement.
        dyn_brake = true;
        braking = true;
        m_brake_l.reset();
        m_brake_r.reset();
        m_speed_pid.reset();
        m_fwd_cmd = 0.f;     // repart en douceur au prochain armement
        m_turn_cmd = 0.f;
        setState((Fault::None != fault) ? State::Fault : State::Lockout, fault);
    }
    else
    {
        m_brake_l.reset();
        m_brake_r.reset();
        float fwd_t = deadzone(in.y, cfg.thr_deadzone);
        const float turn_t = deadzone(in.x, cfg.thr_deadzone);
        if (cfg.allow_reverse == 0.f && fwd_t < 0.f) fwd_t = 0.f;              // recul interdit ?
        else if (fwd_t < 0.f) fwd_t = std::max(fwd_t, -cfg.rev_limit);         // recul BRIDÉ (50 % défaut)

        // 1) Limiteur de pente : interdit une variation trop BRUSQUE (coup de manche « sec »).
        m_fwd_cmd = slew(fwd_t, m_fwd_cmd, cfg.thr_ramp_per_s, hw::CTRL_DT_S);
        m_turn_cmd = slew(turn_t, m_turn_cmd, cfg.turn_rate, hw::CTRL_DT_S);
        fwd = m_fwd_cmd;
        turn = m_turn_cmd;

        // 2) Anti-renversement « rampe » : la limite de virage suit la vitesse MESURÉE.
        //    |v| ≤ turn_full_ms → ±100 % (pivot sur place, v≈0) ; puis décroissance LINÉAIRE
        //    jusqu'à turn_hi (±50 % défaut) atteinte à speed_limit_ms. Sans encodeurs, v=0 → pas de bridage.
        //    Désactivable (turn_limit_en=0) pour les essais au banc.
        if (cfg.turn_limit_en != 0.f)
        {
            const float turn_max = turnLimit(fabsf(v_veh), cfg.turn_full_ms, cfg.speed_limit_ms, cfg.turn_hi);
            turn = clampf(turn, -turn_max, turn_max);
            m_turn_cmd = turn;   // garde l'état borné (pas de windup de la rampe au-delà de la limite)
        }

        // Mix arcade différentiel (pivot sur place possible si fwd≈0).
        mixArcade(fwd, turn, cfg.turn_gain, out_l, out_r);

        // Plafond de vitesse global (préserve le ratio de virage) via PID sur la vitesse moyenne.
        // Sans encodeurs : pas d'asservissement vitesse → on s'appuie sur le plafond PWM (duty_cap).
        if (use_enc && cfg.vlim_enable != 0.f)
        {
            const float vcap = m_speed_pid.update(cfg.speed_limit_ms, fabsf(v_veh), hw::CTRL_DT_S,
                                                  cfg.vmax_kp, cfg.vmax_ki, cfg.vmax_kd, 0.f, 1.f);
            out_l *= vcap;
            out_r *= vcap;
        }
        else
        {
            m_speed_pid.reset();
        }

        if (fabsf(fwd) < 1e-3f && fabsf(turn) < 1e-3f)
        {
            // ARMÉ + manche centré → FREINAGE ACTIF (PID de plugging) si encodeurs présents
            // ET frein PID activé ; sinon repli sur le freinage dynamique (court-circuit).
            braking = true;
            if (use_enc && cfg.brk_pid_enable != 0.f)
            {
                out_l = brakeWheel(m_brake_l, sl, cfg, hw::CTRL_DT_S);
                out_r = brakeWheel(m_brake_r, sr, cfg, hw::CTRL_DT_S);
            }
            else
            {
                dyn_brake = true;
            }
        }
        else
        {
            m_last_act_us = now;
        }
        setState(State::Run, Fault::None);
    }

    if (dyn_brake) board::motorsBrake();                  // court-circuit moteur (freinage dynamique)
    else           board::motorsSet(out_l, out_r, cap);   // pilotage / plugging actif
    if (use_enc)
    {
        updateEncStuck(now, 0.5f * (out_l + out_r), v_veh);
        updateEncSanity(now, braking || dyn_brake, out_l, out_r, sl, sr);
    }

    // Désarmement auto après inactivité.
    if (m_armed && (now - m_last_act_us) > static_cast<int64_t>(cfg.disarm_s) * 1000000) m_armed = false;

    // Coupure d'alimentation si LVC prolongée.
    if (Fault::Lvc == fault && 0 != m_lvc_since_us &&
        (now - m_lvc_since_us) > static_cast<int64_t>(hw::LVC_POWEROFF_MS) * 1000)
    {
        board::powerOff();
    }

    g_status.m_fwd.store(fwd);
    g_status.m_turn.store(turn);
    g_status.m_out_l.store(out_l);
    g_status.m_out_r.store(out_r);
    g_status.m_brake_mode.store(static_cast<int>(
        dyn_brake ? BrakeMode::Dynamic : (braking ? BrakeMode::Active : BrakeMode::None)));
    g_status.m_arming.store(m_armed);
}

void controlTask(void*)
{
    esp_task_wdt_add(nullptr);
    TickType_t last = xTaskGetTickCount();
    while (true)
    {
        tick();
        esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(hw::CTRL_DT_MS));
    }
}
} // namespace

void Controller::init()
{
    board::init();
    input::init();
    setState(State::Lockout, Fault::None);
    g_status.m_brake_mode.store(static_cast<int>(BrakeMode::Dynamic));   // défaut : freinage dynamique (jamais en roue libre)
}

void Controller::start()
{
    xTaskCreatePinnedToCore(controlTask, rtos::CONTROL.name, rtos::CONTROL.stack, nullptr,
                            rtos::CONTROL.prio, nullptr, rtos::CONTROL.core);
}
