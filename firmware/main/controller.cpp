// controller.cpp — Contrôle DIFFÉRENTIEL (variante expérimentale).
// 2 roues avant motrices indépendantes + 1 roulette arrière. Pilotage manette (namespace input).
// Mix arcade : y = avance, x = virage → gauche = avance − virage, droite = avance + virage.
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

constexpr float PI_F = 3.14159265358979f;

// Δcounts AS5600 (12 bits, 4096/tour) sur un tick → km/h SIGNÉE (le signe donne le sens).
float countsToKmh(int delta)
{
    const float sensor_rps = (static_cast<float>(delta) / hw::AS5600_CPR) * hw::CTRL_HZ;
    const float wheel_rps = sensor_rps / hw::GEAR_RATIO;
    return wheel_rps * PI_F * hw::WHEEL_DIAM_M * 3.6f;
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
float   m_fwd_cmd = 0.f;   // consigne avance APRÈS limiteur de pente (état conservé entre ticks)
float   m_turn_cmd = 0.f;  // consigne virage APRÈS limiteur de pente
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

void updateLVC(float vbat, const KartConfig& cfg)
{
    const int64_t now = esp_timer_get_time();
    if (!m_lvc_tripped)
    {
        if (vbat < cfg.vbat_cut_v)
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
    else if (vbat > cfg.vbat_recover_v)
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        m_lvc_since_us = 0;
    }
}

// Freine une roue : PID vitesse → 0 (sortie signée, peut inverser). Renvoie la commande appliquée.
float brakeWheel(Pid& pid, float speed_kmh, const KartConfig& cfg, float dt)
{
    if (fabsf(speed_kmh) <= hw::EBRAKE_MIN_KMH)
    {
        pid.reset();
        return 0.f;
    }
    return pid.update(0.f, speed_kmh, dt, cfg.pid_kp, cfg.pid_ki, cfg.pid_kd, -1.f, 1.f);
}

void updateEncStuck(int64_t now, float cmd, float speed_kmh)
{
    if (fabsf(cmd) > hw::ENC_STUCK_PWM && fabsf(speed_kmh) <= 0.05f)
    {
        if (0 == m_stuck_us) m_stuck_us = now;
        else if ((now - m_stuck_us) > static_cast<int64_t>(hw::ENC_STUCK_MS) * 1000) m_enc_fault = true;
    }
    else
    {
        m_stuck_us = 0;
    }
}

void publish(float sl, float sr, float vbat, const input::State& in)
{
    g_status.m_vbat.store(vbat);
    g_status.m_speed_l.store(sl);
    g_status.m_speed_r.store(sr);
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
    const float vraw = board::vbatVolts(hw::ADC_OVERSAMPLE);   // < 0 si capteur (ADS1115) absent
    const bool  vbat_valid = (vraw > 0.05f);
    const float vbat = vbat_valid ? vraw * cfg.vbat_div_ratio : 0.f;
    const float sl_raw = use_enc ? countsToKmh(board::encLeftDelta())  : 0.f;
    const float sr_raw = use_enc ? countsToKmh(board::encRightDelta()) : 0.f;
    // Lissage EMA : atténue la quantification du Δangle par tick à basse vitesse.
    m_sl_ema += hw::SPEED_EMA_ALPHA * (sl_raw - m_sl_ema);
    m_sr_ema += hw::SPEED_EMA_ALPHA * (sr_raw - m_sr_ema);
    const float sl = m_sl_ema, sr = m_sr_ema;
    const float v_avg = 0.5f * (sl + sr);
    if (!use_enc) m_enc_fault = false;   // pas d'encodeurs → pas de défaut « capteur bloqué »

    // Capteur de tension absent → tension inconnue : on NE déclenche PAS la LVC (le BMS du
    // pack assure la protection). Permet aussi de tester au banc sans l'ADS1115 câblé.
    if (vbat_valid)
    {
        updateLVC(vbat, cfg);
    }
    else
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        m_lvc_since_us = 0;
    }
    publish(sl, sr, vbat, in);

    const uint32_t cap = static_cast<uint32_t>(hw::PWM_MAX * clampf(cfg.duty_cap_frac, 0.f, 1.f));

    // ── Défauts / conditions de non-conduite ──
    Fault fault = Fault::None;
    if (m_lvc_tripped)                              fault = Fault::Lvc;
    else if (m_enc_fault)                           fault = Fault::Encoder;
    else if (in.connected && !input::calibrated())  fault = Fault::NotCalibrated;  // manette non calibrée

    // Manette absente / e-stop manette / DÉFAUT → on désarme et on freine (sécurité absolue).
    // Un défaut force le désarmement : il faudra réarmer (START maintenu) une fois résolu.
    if (!in.connected || in.estop || (Fault::None != fault)) m_armed = false;

    const bool can_drive = m_armed && in.connected && !in.estop && (Fault::None == fault);

    // ── Armement par appui maintenu sur START (anti-démarrage : manche centré + manette connectée) ──
    // START = bouton physique OU bouton START/Options de la manette (même fonction).
    const bool start_held = board::btnStart() || in.start;
    const bool centered = (fabsf(in.x) < 0.08f) && (fabsf(in.y) < 0.08f);
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
    const bool hard_fault = (Fault::Lvc == fault) || (Fault::Encoder == fault);
    if (m_armed && !m_armed_prev)                               // vient d'être armé → doux
        input::rumble(90, 160, 220);
    if ((hard_fault && !m_hardfault_prev) || (in.estop && !m_estop_prev))  // erreur soudaine / e-stop → fort
        input::rumble(255, 255, 450);
    const bool pushing = (fabsf(in.rx) > 0.5f) || (fabsf(in.ry) > 0.5f);
    if (pushing && !can_drive && (now - m_rumble_block_us) > 800000)       // bouge mais bloqué → fort (répété)
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
        if (cfg.allow_reverse == 0.f && fwd_t < 0.f) fwd_t = 0.f;   // recul interdit ?

        // 1) Limiteur de pente : interdit une variation trop BRUSQUE (coup de manche « sec »).
        m_fwd_cmd = slew(fwd_t, m_fwd_cmd, cfg.thr_ramp_per_s, hw::CTRL_DT_S);
        m_turn_cmd = slew(turn_t, m_turn_cmd, cfg.turn_rate, hw::CTRL_DT_S);
        fwd = m_fwd_cmd;
        turn = m_turn_cmd;

        // 2) Anti-renversement : borne l'AMPLITUDE du virage pour que a_lat = v·ω reste
        //    ≤ a_lat_max (prédictif). a_lat ≈ 2·turn_gain·Vmax²·|fwd|·|turn| / voie.
        const float vmax_mps = std::max(cfg.speed_limit_kmh, 1.f) / 3.6f;
        const float denom = 2.f * std::max(cfg.turn_gain, 0.01f) * vmax_mps * vmax_mps * fabsf(fwd);
        float turn_max = 1.f;
        if (denom > 1e-3f) turn_max = clampf(cfg.a_lat_max * hw::TRACK_M / denom, 0.f, 1.f);
        turn = clampf(turn, -turn_max, turn_max);
        m_turn_cmd = turn;   // garde l'état borné (pas de windup de la rampe au-delà de la limite)

        // Mix arcade différentiel (pivot sur place possible si fwd≈0).
        mixArcade(fwd, turn, cfg.turn_gain, out_l, out_r);

        // Plafond de vitesse global (préserve le ratio de virage) via PID sur la vitesse moyenne.
        // Sans encodeurs : pas d'asservissement vitesse → on s'appuie sur le plafond PWM (duty_cap).
        if (use_enc)
        {
            const float vcap = m_speed_pid.update(cfg.speed_limit_kmh, fabsf(v_avg), hw::CTRL_DT_S,
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
            // ARMÉ + manche centré → FREINAGE ACTIF (PID de plugging) si encodeurs présents ;
            // sinon repli sur le freinage dynamique (court-circuit).
            braking = true;
            if (use_enc)
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
    if (use_enc) updateEncStuck(now, 0.5f * (out_l + out_r), v_avg);

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
    g_status.m_brake.store(braking);
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
    g_status.m_brake.store(true);   // état par défaut : FREINAGE (jamais en roue libre au repos)
}

void Controller::start()
{
    xTaskCreatePinnedToCore(controlTask, rtos::CONTROL.name, rtos::CONTROL.stack, nullptr,
                            rtos::CONTROL.prio, nullptr, rtos::CONTROL.core);
}
