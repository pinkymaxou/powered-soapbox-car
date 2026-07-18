// controller_core.cpp — Logique de contrôle du kart (voir controller_core.hpp).
// PURE : transplantée 1:1 depuis l'ancien controller.cpp ; toute E/S passe par les io*.
// Mix arcade : y = avance, x = virage → gauche = avance + virage, droite = avance − virage.
// Sécurités : manette déconnectée/muette, e-stop, LVC, défauts encodeur → FREINAGE.
// Anti-renversement : le virage autorisé décroît avec la vitesse mesurée.
#include "controller_core.hpp"

#include <algorithm>
#include <cmath>

namespace
{
using ctl::clampf;
using ctl::deadzone;
using ctl::mixArcade;
using ctl::slew;
using ctl::turnLimit;
} // namespace

void KartController::updateLVC(float vbat, int64_t now)
{
    // Seuils codés en dur selon la batterie DÉTECTÉE au démarrage (12 V ou 24 V, plomb).
    // Tant que la tension n'a pas été stable 3 s (type inconnu) : pas de LVC — le kart
    // démarre désarmé de toute façon, et on ne change jamais de batterie système allumé.
    const int bt = m_batt_det.volts;
    if (0 == bt)
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        return;
    }
    const float cut_v     = (24 == bt) ? hw::VBAT24_CUT_V     : hw::VBAT12_CUT_V;
    const float recover_v = (24 == bt) ? hw::VBAT24_RECOVER_V : hw::VBAT12_RECOVER_V;

    if (!m_lvc_tripped)
    {
        if (vbat < cut_v)
        {
            if (0 == m_sag_start_us) m_sag_start_us = now;
            if ((now - m_sag_start_us) > static_cast<int64_t>(hw::VBAT_SAG_DEBOUNCE_MS) * 1000)
            {
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
    }
}

// Freine une roue : PID vitesse → 0 (sortie signée, peut inverser). Vitesse en m/s.
float KartController::brakeWheel(Pid& pid, float speed_ms, const KartConfig& cfg, float dt)
{
    if (std::fabs(speed_ms) <= hw::EBRAKE_MIN_MPS)
    {
        pid.reset();
        return 0.f;
    }
    return pid.update(0.f, speed_ms, dt, cfg.brk_kp, cfg.brk_ki, cfg.brk_kd, -1.f, 1.f);
}

void KartController::updateEncStuck(int64_t now, float cmd, float speed_ms)
{
    if (std::fabs(cmd) > hw::ENC_STUCK_PWM && std::fabs(speed_ms) <= 0.05f)
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
void KartController::updateEncSanity(int64_t now, bool braking, float out_l, float out_r,
                                     float sl, float sr)
{
    // RevDetect distingue une VRAIE inversion (vitesse opposée stable/croissante) d'une
    // DÉCÉLÉRATION commandée — freinage au stick — où la vitesse opposée fond vers zéro.
    if (!braking)
    {
        constexpr int64_t win = static_cast<int64_t>(hw::ENC_REV_MS) * 1000;
        if (m_rev_l.update(out_l, sl, now, win, hw::ENC_REV_PWM, hw::ENC_REV_MPS, hw::ENC_REV_DECAY_MPS) ||
            m_rev_r.update(out_r, sr, now, win, hw::ENC_REV_PWM, hw::ENC_REV_MPS, hw::ENC_REV_DECAY_MPS))
        {
            m_enc_rev_fault = true;
        }
    }
    else
    {
        m_rev_l.reset();
        m_rev_r.reset();
    }

    if (std::fabs(sl) > hw::ENC_MAX_SANE_MPS || std::fabs(sr) > hw::ENC_MAX_SANE_MPS)
    {
        if (0 == m_mad_us) m_mad_us = now;
        else if ((now - m_mad_us) > static_cast<int64_t>(hw::ENC_MAD_MS) * 1000) m_enc_mad_fault = true;
    }
    else
    {
        m_mad_us = 0;
    }
}

// step() — TOUTE la logique métier d'un pas. Tourne SOUS le verrou pris par tick().
CtrlOutputs KartController::step(const CtrlInputs& in)
{
    CtrlOutputs out;
    const int64_t now = in.now_us;

    const bool use_enc = (m_cfg.use_encoders != 0.f);   // 0 = ignore les AS5600 (banc sans encodeurs)

    // Heartbeat : « connectée » mais aucun rapport HID depuis 250 ms → traitée comme
    // DÉCONNECTÉE (désarmement + freinage immédiats, sans attendre le timeout Bluetooth).
    const bool pad_stale = in.pad.connected &&
                           ((now - in.pad.last_report_us) > hw::PAD_HB_TIMEOUT_US);
    // Tension lue à 20 Hz seulement (hw::VBAT_READ_TICKS) : l'ADS1115 à 128 SPS ne produit
    // rien de neuf plus vite, et ça évite ~4000 transactions I2C/s dans la boucle 500 Hz.
    if (0 == (m_vbat_tick++ % hw::VBAT_READ_TICKS))
    {
        m_vraw = in.sensors.vbat_ok ? in.sensors.vbat_v : -1.f;
    }
    const bool  vbat_valid = (m_vraw > 0.05f);
    const float vbat = vbat_valid ? m_vraw : 0.f;   // déjà en volts batterie (hôte)
    // Taux de counts (counts/s), lissé par EMA (atténue la quantification du Δangle par
    // tick à basse vitesse) puis converti par les RATIOS DE CONFIG (enc_mps_per_cps /
    // enc_rpm_per_cps) — le cœur ne connaît ni le CPR, ni la réduction, ni la roue.
    const float cps_l = use_enc ? static_cast<float>(in.sensors.enc_delta_l) * hw::CTRL_HZ : 0.f;
    const float cps_r = use_enc ? static_cast<float>(in.sensors.enc_delta_r) * hw::CTRL_HZ : 0.f;
    m_cps_l += hw::SPEED_EMA_ALPHA * (cps_l - m_cps_l);
    m_cps_r += hw::SPEED_EMA_ALPHA * (cps_r - m_cps_r);
    const float sl = m_cps_l * m_cfg.enc_mps_per_cps;   // vitesses roues SIGNÉES (m/s)
    const float sr = m_cps_r * m_cfg.enc_mps_per_cps;
    // Vitesse VÉHICULE (m/s) = moyenne signée des deux roues : deux roues égales en sens
    // inverse (pivot sur place) → 0 m/s. C'est elle qui sert aux ajustements de conduite.
    const float v_veh = 0.5f * (sl + sr);
    if (!use_enc)
    {
        m_enc_fault = false;   // pas d'encodeurs → pas de défaut « capteur »
        m_enc_rev_fault = false;
        m_enc_mad_fault = false;
        m_rev_l.reset();
        m_rev_r.reset();
        m_mad_us = 0;
    }

    // Capteur de tension absent → tension inconnue : on NE déclenche PAS la LVC (le BMS du
    // pack assure la protection). Permet aussi de tester au banc sans l'ADS1115 câblé.
    if (vbat_valid)
    {
        m_batt_det.update(vbat, now, hw::VBAT_DETECT_STABLE_US,
                          hw::VBAT_DETECT_TOL_V, hw::VBAT_DETECT_24V_MIN);
        updateLVC(vbat, now);
        // Lissage LENT pour le plafond PWM auto : l'affaissement sous charge ne doit pas
        // faire osciller le duty (sag → Vbat baisse → duty remonte → plus de sag…).
        if (m_vbat_ema <= 0.f) m_vbat_ema = vbat;
        else m_vbat_ema += hw::VBAT_CAP_EMA_ALPHA * (vbat - m_vbat_ema);
    }
    else
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        m_vbat_ema = 0.f;   // tension inconnue → pas de plafond automatique
    }

    // Plafond PWM : AUTOMATIQUE (12 V nominaux / Vbat mesurée : 12 V → ~100 %, 24 V → ~50 %)
    // ET manuel (duty_cap, page web) — le plus restrictif gagne. Sans ADS1115 : manuel seul.
    const float duty_max = std::min(m_cfg.duty_cap_frac, ctl::dutyCapVolts(m_vbat_ema, hw::MOTOR_V_NOM));
    const uint32_t cap = static_cast<uint32_t>(hw::PWM_MAX * clampf(duty_max, 0.f, 1.f));

    // ── Défauts / conditions de non-conduite ──
    // Encodeurs ABSENTS (I2C muet) : avec use_encoders=1 c'est bloquant — frein PID et
    // limiteur croiraient la roue arrêtée. Avec use_encoders=0 (banc) : simplement ignorés.
    const bool enc_l_abs = use_enc && !in.sensors.enc_ok_l;
    const bool enc_r_abs = use_enc && !in.sensors.enc_ok_r;

    // BITSET de TOUTES les erreurs/conditions actives — l'unique représentation des
    // défauts du cœur. fb::BLOCKING interdit la conduite ; le défaut prioritaire pour
    // l'affichage se dérive avec primaryFault(). Bits : voir FAULTS_DESC (index.html).
    unsigned fmask = 0;
    if (in.pad.estop)                                fmask |= fb::ESTOP;
    if (m_lvc_tripped)                           fmask |= fb::LVC;
    if (in.pad.connected && !in.pad.calibrated)          fmask |= fb::NOCAL;
    if (m_enc_fault)                             fmask |= fb::ENC_STUCK;
    if (!in.pad.connected)                           fmask |= fb::PAD_LOST;
    if (pad_stale)                               fmask |= fb::PAD_STALE;
    if (!vbat_valid)                             fmask |= fb::NO_VBAT;
    if (m_enc_rev_fault)                         fmask |= fb::ENC_REV;
    if (m_enc_mad_fault)                         fmask |= fb::ENC_MAD;
    if (enc_l_abs)                               fmask |= fb::ENC_L_ABS;
    if (enc_r_abs)                               fmask |= fb::ENC_R_ABS;

    const bool blocking = (0 != (fmask & fb::BLOCKING));

    // Manette absente / e-stop manette / DÉFAUT BLOQUANT → on désarme et on freine (sécurité
    // absolue). Un défaut force le désarmement : réarmement (START maintenu) une fois résolu.
    if (!in.pad.connected || pad_stale || in.pad.estop || blocking) m_armed = false;

    const bool can_drive = m_armed && in.pad.connected && !pad_stale && !in.pad.estop && !blocking;

    // ── Armement par appui maintenu sur START (anti-démarrage : manche centré + manette connectée) ──
    // START = bouton physique OU bouton START/Options de la manette (même fonction).
    const bool start_held = in.btn_start_hw || in.pad.start;
    const bool centered = (std::fabs(in.pad.x) < hw::ARM_CENTER_MAX) && (std::fabs(in.pad.y) < hw::ARM_CENTER_MAX);
    if (start_held)
    {
        if (0 == m_hold_start_us) m_hold_start_us = now;
        else if (!m_start_latch && (now - m_hold_start_us) > static_cast<int64_t>(m_cfg.arm_hold_ms) * 1000)
        {
            m_start_latch = true;
            if (!m_armed && in.pad.connected && !pad_stale && centered && !blocking)
            {
                m_armed = true;
                m_last_act_us = now;
            }
            else
            {
                m_armed = false;
            }
        }
    }
    else
    {
        m_hold_start_us = 0;
        m_start_latch = false;
    }

    float out_l = 0.f, out_r = 0.f, fwd = 0.f, turn = 0.f;
    bool braking = false;
    bool dyn_brake = false;   // true → court-circuit moteur (freinage dynamique passif)
    State state = State::Run;

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
        state = blocking ? State::Fault : State::Lockout;
    }
    else
    {
        m_brake_l.reset();
        m_brake_r.reset();
        float fwd_t = deadzone(in.pad.y, m_cfg.thr_deadzone);
        const float turn_t = deadzone(in.pad.x, m_cfg.thr_deadzone);
        if (m_cfg.allow_reverse == 0.f && fwd_t < 0.f) fwd_t = 0.f;              // recul interdit ?
        else if (fwd_t < 0.f) fwd_t = std::max(fwd_t, -m_cfg.rev_limit);         // recul BRIDÉ (50 % défaut)

        // 1) Limiteur de pente : interdit une variation trop BRUSQUE (coup de manche « sec »).
        m_fwd_cmd = slew(fwd_t, m_fwd_cmd, m_cfg.thr_ramp_per_s, hw::CTRL_DT_S);
        m_turn_cmd = slew(turn_t, m_turn_cmd, m_cfg.turn_rate, hw::CTRL_DT_S);
        fwd = m_fwd_cmd;
        turn = m_turn_cmd;

        // 2) Anti-renversement « rampe » : la limite de virage suit la vitesse MESURÉE.
        //    |v| ≤ turn_full_ms → ±100 % (pivot sur place, v≈0) ; puis décroissance LINÉAIRE
        //    jusqu'à turn_hi (±50 % défaut) atteinte à speed_limit_ms. Sans encodeurs, v=0 → pas de bridage.
        //    Désactivable (turn_limit_en=0) pour les essais au banc.
        if (m_cfg.turn_limit_en != 0.f)
        {
            const float turn_max = turnLimit(std::fabs(v_veh), m_cfg.turn_full_ms, m_cfg.speed_limit_ms, m_cfg.turn_hi);
            turn = clampf(turn, -turn_max, turn_max);
            m_turn_cmd = turn;   // garde l'état borné (pas de windup de la rampe au-delà de la limite)
        }

        // Mix arcade différentiel (pivot sur place possible si fwd≈0).
        mixArcade(fwd, turn, m_cfg.turn_gain, out_l, out_r);

        // Plafond de vitesse global (préserve le ratio de virage) via PID sur la vitesse moyenne.
        // Sans encodeurs : pas d'asservissement vitesse → on s'appuie sur le plafond PWM (duty_cap).
        if (use_enc && m_cfg.vlim_enable != 0.f)
        {
            const float vcap = m_speed_pid.update(m_cfg.speed_limit_ms, std::fabs(v_veh), hw::CTRL_DT_S,
                                                  m_cfg.vlim_kp, m_cfg.vlim_ki, m_cfg.vlim_kd, 0.f, 1.f);
            out_l *= vcap;
            out_r *= vcap;
        }
        else
        {
            m_speed_pid.reset();
        }

        if (std::fabs(fwd) < 1e-3f && std::fabs(turn) < 1e-3f)
        {
            // ARMÉ + manche centré → FREINAGE ACTIF (PID de plugging) si encodeurs présents
            // ET frein PID activé ; sinon repli sur le freinage dynamique (court-circuit).
            braking = true;
            if (use_enc && m_cfg.brk_pid_enable != 0.f)
            {
                out_l = brakeWheel(m_brake_l, sl, m_cfg, hw::CTRL_DT_S);
                out_r = brakeWheel(m_brake_r, sr, m_cfg, hw::CTRL_DT_S);
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
        state = State::Run;
    }

    out.dyn_brake = dyn_brake;   // court-circuit moteur, sinon pilotage / plugging actif
    out.out_l = out_l;
    out.out_r = out_r;
    out.cap = cap;
    if (use_enc)
    {
        updateEncStuck(now, 0.5f * (out_l + out_r), v_veh);
        updateEncSanity(now, braking || dyn_brake, out_l, out_r, sl, sr);
    }

    // Désarmement auto après inactivité.
    if (m_armed && (now - m_last_act_us) > static_cast<int64_t>(m_cfg.disarm_s) * 1000000) m_armed = false;

    // ── Télémétrie du tick ──
    m_tel.state      = state;
    m_tel.faults     = fmask;
    m_tel.vbat       = vbat;
    m_tel.batt_type  = m_batt_det.volts;
    m_tel.speed_l    = sl;
    m_tel.speed_r    = sr;
    m_tel.rpm_l      = m_cps_l * m_cfg.enc_rpm_per_cps;
    m_tel.rpm_r      = m_cps_r * m_cfg.enc_rpm_per_cps;
    m_tel.speed_ms   = v_veh;
    m_tel.fwd        = fwd;
    m_tel.turn       = turn;
    m_tel.out_l      = out_l;
    m_tel.out_r      = out_r;
    m_tel.brake_mode = dyn_brake ? BrakeMode::Dynamic : (braking ? BrakeMode::Active : BrakeMode::None);
    m_tel.armed      = m_armed;
    m_tel.btn_start  = in.btn_start_hw || in.pad.start;

    return out;
}

// ── Enveloppe publique THREAD-SAFE ─────────────────────────────────────────
// Les entrées/lectures verrouillent brièvement ; tick() déroule step() sous verrou mais
// appelle les callbacks HORS verrou (pas d'interblocage si l'hôte relit le contrôleur).

void KartController::setCallbacks(UpdateSensorsFn updateSensors, UpdateOutputsFn updateOutputs)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_update_sensors = std::move(updateSensors);
    m_update_outputs = std::move(updateOutputs);
}

void KartController::setConfig(const KartConfig& cfg)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_cfg = cfg;
}

KartConfig KartController::config() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_cfg;
}

void KartController::setPad(const PadInputs& pad)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_pad = pad;
}

void KartController::setStartButton(bool held)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_btn_start_hw = held;
}

CtrlTelemetry KartController::telemetry() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_tel;
}

CtrlOutputs KartController::tick(int64_t now_us)
{
    UpdateSensorsFn read;
    UpdateOutputsFn apply;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        read = m_update_sensors;
        apply = m_update_outputs;
    }
    CtrlInputs in;
    in.now_us = now_us;
    in.sensors = read ? read() : SensorReadings{};   // sans câblage : capteurs « absents »
    CtrlOutputs out;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        in.pad = m_pad;
        in.btn_start_hw = m_btn_start_hw;
        out = step(in);
    }
    if (apply) apply(out);
    return out;
}
