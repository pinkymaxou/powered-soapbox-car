// controller_core.cpp — Kart control logic (see controller_core.hpp).
// PURE: transplanted 1:1 from the old controller.cpp; all I/O goes through the io*.
// Arcade mixing: y = forward, x = turn → left = forward + turn, right = forward − turn.
// Safeties: gamepad disconnected/silent, e-stop, LVC, encoder faults → BRAKING.
// Rollover protection: the allowed turn decreases with the measured speed.
#include "controller_core.hpp"

#include <algorithm>
#include <cmath>

#include "mixer.hpp"

namespace
{
using ctl::clampf;
using ctl::deadzone;
using ctl::mixArcade;
using ctl::slew;
using ctl::turnLimit;

// Median of 5 — rejects isolated outliers (a corrupted I2C read, or an aliased Δ when the loop
// was delayed and the shaft moved > half a turn between reads) without averaging lag.
float median5(const float w[5]) { float a[5]; std::copy(w, w + 5, a); std::sort(a, a + 5); return a[2]; }
} // namespace

void KartController::updateLVC(float vbat, int64_t now)
{
    // Thresholds hard-coded according to the battery DETECTED at startup (12 V or 24 V, lead-acid).
    // As long as the voltage has not been stable 3 s (unknown type): no LVC — the kart
    // starts disarmed anyway, and one never changes battery with the system powered on.
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

// Brakes one wheel: speed PID → 0 (signed output, can reverse). Speed in m/s.
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

// Encoder sanity: reversed direction (wheel measured opposite to a firm command —
// sensor OR motor wired backwards) and aberrant measurement (impossible speed). A sensor
// that LIES makes PID braking and the limiter DANGEROUS (they would push instead of hold) →
// TOTAL STOP, latched until restart. "braking" excludes PID braking: it opposes
// rotation by design and would trigger the direction test wrongly.
void KartController::updateEncSanity(int64_t now, bool braking, float out_l, float out_r,
                                     float sl, float sr)
{
    // RevDetect distinguishes a REAL reversal (opposed speed, stable/growing) from a
    // commanded DECELERATION — braking at the stick — where the opposed speed melts toward zero.
    // OPTIONAL (enc_rev_chk): its blind spot is plugging on a downhill — reverse stick while
    // the slope holds the speed up looks exactly like a reversed sensor, and the latched full
    // stop mid-descent is worse than what it guards against. An owner who verifies the rpm
    // signs at commissioning (Dashboard, push the kart forward) can turn the watchdog off;
    // ENC_MAD and ENC_STUCK stay as the runtime backstops either way.
    if (!braking && 0 != m_cfg.enc_rev_chk)
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

// step() — ALL the business logic of a step. Runs UNDER the lock taken by tick().
CtrlOutputs KartController::step(const CtrlInputs& in)
{
    CtrlOutputs out;
    const int64_t now = in.now_us;

    const bool use_enc = (m_cfg.use_encoders != 0);   // 0 = ignore the AS5600 (bench without encoders)

    // Heartbeat: "connected" but no HID report for 250 ms → treated as
    // DISCONNECTED (immediate disarm + braking, without waiting for the Bluetooth timeout).
    const bool pad_stale = in.pad.connected &&
                           ((now - in.pad.last_report_us) > hw::PAD_HB_TIMEOUT_US);
    // Voltage read at 20 Hz only (hw::VBAT_READ_TICKS): the ADS1115 at 128 SPS produces
    // nothing new any faster, and it avoids ~4000 I2C transactions/s in the 500 Hz loop.
    if (0 == (m_vbat_tick++ % hw::VBAT_READ_TICKS))
    {
        m_vraw = in.sensors.vbat_ok ? in.sensors.vbat_v : -1.f;
    }
    const bool  vbat_valid = (m_vraw > 0.05f);
    const float vbat = vbat_valid ? m_vraw : 0.f;   // already in battery volts (host)
    // Count rate (counts/s), smoothed by EMA (attenuates the quantization of the Δangle per
    // tick at low speed) then converted by the CONFIG RATIOS (enc_mps_per_cps /
    // enc_rpm_per_cps) — the core knows neither the CPR, nor the reduction, nor the wheel.
    // The encoders are ALWAYS read and published (bench gearbox testing: watch the rpm even
    // disarmed or with use_encoders=0). use_encoders only decides whether the CONTROL loop
    // TRUSTS them (speed limiter, brake PID, rollover limit, encoder faults) — not the display.
    // Count rate over the REAL elapsed time (esp_timer), not a fixed 2 ms tick: the encoder
    // read sits after variable per-tick work, so the read spacing jitters — dividing Δcounts
    // by the actual interval removes that ripple. Then 5-sample MEDIAN (spike reject) → EMA.
    float dt_s = (m_last_now_us != 0) ? (now - m_last_now_us) * 1e-6f : hw::CTRL_DT_S;
    m_last_now_us = now;
    if (dt_s <= 0.f || dt_s > 0.1f) dt_s = hw::CTRL_DT_S;   // first tick / clock glitch → nominal
    const float inv_dt = 1.f / dt_s;
    // Per-wheel SIGN. Which way an AS5600 counts depends on which face of the magnet it sees,
    // so it flips with the motor's mounting orientation — and the two sides are mirrored to
    // begin with. CONVENTION: positive rpm = FORWARD, negative = reverse, on both wheels.
    // enc_inv_* corrects the mounting in software instead of rewiring; the ENC_REV fault stays
    // as the safety net for the case where it is set wrong.
    const float sign_l = (0 != m_cfg.enc_inv_l) ? -1.f : 1.f;
    const float sign_r = (0 != m_cfg.enc_inv_r) ? -1.f : 1.f;
    m_cps_win_l[m_cps_win_i] = sign_l * static_cast<float>(in.sensors.enc_delta_l) * inv_dt;
    m_cps_win_r[m_cps_win_i] = sign_r * static_cast<float>(in.sensors.enc_delta_r) * inv_dt;
    m_cps_win_i = (m_cps_win_i + 1) % 5;
    const float cps_l = median5(m_cps_win_l);
    const float cps_r = median5(m_cps_win_r);
    m_cps_l += hw::SPEED_EMA_ALPHA * (cps_l - m_cps_l);
    m_cps_r += hw::SPEED_EMA_ALPHA * (cps_r - m_cps_r);
    const float sl = m_cps_l * m_cfg.enc_mps_per_cps;   // SIGNED wheel speeds (m/s), measured
    const float sr = m_cps_r * m_cfg.enc_mps_per_cps;
    const float v_meas = 0.5f * (sl + sr);   // measured vehicle speed (m/s) — for telemetry
    // CONTROL speed: forced to 0 without encoders so nothing (limiter / brake / rollover
    // limit) trusts them. Pivot in place (two opposite wheels) also averages to ~0.
    const float v_veh = use_enc ? v_meas : 0.f;
    if (!use_enc)
    {
        m_enc_fault = false;   // no encoders → no "sensor" fault
        m_enc_rev_fault = false;
        m_enc_mad_fault = false;
        m_rev_l.reset();
        m_rev_r.reset();
        m_mad_us = 0;
    }

    // Voltage sensor absent → unknown voltage: we do NOT trigger the LVC (the pack's BMS
    // ensures protection). Also allows bench testing without the ADS1115 wired.
    if (vbat_valid)
    {
        // Type detection stays on the RAW value: it only runs at rest, before any load, and
        // deliberately demands a voltage that does not move.
        m_batt_det.update(vbat, now, hw::VBAT_DETECT_STABLE_US,
                          hw::VBAT_DETECT_TOL_V, hw::VBAT_DETECT_24V_MIN);
        // The LVC judges a SLOWLY SMOOTHED voltage, so an acceleration sag cannot trip it —
        // see hw::VBAT_LVC_EMA_* for why the raw value was the wrong thing to threshold.
        if (m_vbat_lvc <= 0.f) m_vbat_lvc = vbat;
        else m_vbat_lvc += hw::VBAT_LVC_EMA_ALPHA * (vbat - m_vbat_lvc);
        // vbat_check_en = 0: the voltage stays MEASURED (display, graphs, automatic PWM cap
        // below) but stops being a driving condition — no fb::LVC, so nothing blocks and the
        // 30 s power cutoff (which watches that very bit) never arms either.
        if (m_cfg.vbat_check_en != 0)
        {
            updateLVC(m_vbat_lvc, now);
        }
        else
        {
            m_lvc_tripped = false;
            m_sag_start_us = 0;
        }
        // SLOW smoothing for the auto PWM cap: the sag under load must not
        // make the duty oscillate (sag → Vbat drops → duty rises → more sag…).
        if (m_vbat_ema <= 0.f) m_vbat_ema = vbat;
        else m_vbat_ema += hw::VBAT_CAP_EMA_ALPHA * (vbat - m_vbat_ema);
    }
    else
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        m_vbat_ema = 0.f;   // unknown voltage → no automatic cap
        m_vbat_lvc = 0.f;   // and the LVC filter restarts from the next real reading
    }

    // PWM cap: AUTOMATIC (12 V nominal / measured Vbat: 12 V → ~100%, 24 V → ~50%)
    // AND manual (duty_cap, web page) — the most restrictive wins. Without ADS1115: manual only.
    const float duty_max = std::min(m_cfg.duty_cap_frac, ctl::dutyCapVolts(m_vbat_ema, hw::MOTOR_V_NOM));
    const uint32_t cap = static_cast<uint32_t>(hw::PWM_MAX * clampf(duty_max, 0.f, 1.f));

    // ── Faults / non-driving conditions ──
    // ABSENT encoders (I2C silent): with use_encoders=1 it is blocking — PID braking and
    // the limiter would think the wheel is stopped. With use_encoders=0 (bench): simply ignored.
    // Encoder SENSOR status — REPORTED regardless of use_encoders (so the bench sees it with
    // use_encoders=0); only BLOCKS driving when use_encoders=1 (see `blocking` below).
    const bool enc_l_abs = !in.sensors.enc_ok_l;   // I2C silent (chip absent/unplugged)
    const bool enc_r_abs = !in.sensors.enc_ok_r;
    const bool mag_l_out = in.sensors.enc_ok_l && !in.sensors.mag_ok_l;  // chip present, magnet out of field
    const bool mag_r_out = in.sensors.enc_ok_r && !in.sensors.mag_ok_r;

    // BITSET of ALL active errors/conditions — the sole representation of the
    // core's faults. fb::BLOCKING forbids driving; the priority fault for
    // display is derived with primaryFault(). Bits: see FAULTS_DESC (index.html).
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
    if (mag_l_out)                               fmask |= fb::MAG_L;
    if (mag_r_out)                               fmask |= fb::MAG_R;
    // Motor rail dead while the logic rail is alive = the emergency stop is pressed (two-rail
    // wiring). Reported only when the opto is declared wired, and blocking on the same terms:
    // it must force a DISARM, so that releasing the mushroom button never resumes drive on its
    // own — the driver has to hold START again.
    const bool no_motor_pwr = (0 != m_cfg.pwr_sense_en) && !in.sensors.motor_pwr;
    if (no_motor_pwr)                            fmask |= fb::NO_MOTOR_PWR;

    const bool blocking = (0 != (fmask & fb::BLOCKING)) || no_motor_pwr ||
                          (use_enc && (enc_l_abs || enc_r_abs || mag_l_out || mag_r_out));

    // Gamepad absent / gamepad e-stop / BLOCKING FAULT → we disarm and brake (absolute
    // safety). A fault forces disarming: re-arm (START held) once resolved.
    if (!in.pad.connected || pad_stale || in.pad.estop || blocking) m_armed = false;

    const bool can_drive = m_armed && in.pad.connected && !pad_stale && !in.pad.estop && !blocking;

    // ── Arming by held press on START (anti-startup: stick centered + gamepad connected) ──
    // START = physical button OR the gamepad's START/Options button (same function).
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
    bool dyn_brake = false;   // true → motor short-circuit (passive dynamic braking)
    State state = State::Run;

    if (!can_drive)
    {
        // NOT ARMED / fault / e-stop / gamepad disconnected → DYNAMIC BRAKING (motor
        // short-circuit). Default resting state; requires neither encoders nor control loop.
        dyn_brake = true;
        braking = true;
        m_brake_l.reset();
        m_brake_r.reset();
        m_speed_pid.reset();
        m_fwd_cmd = 0.f;     // restarts smoothly at the next arming
        m_turn_cmd = 0.f;
        state = blocking ? State::Fault : State::Lockout;
    }
    else
    {
        m_brake_l.reset();
        m_brake_r.reset();
        float fwd_t = deadzone(in.pad.y, m_cfg.thr_deadzone);
        const float turn_t = deadzone(in.pad.x, m_cfg.thr_deadzone);
        // Reverse is always allowed; rev_speed_ms is what holds it back.
        // (No PWM limit in reverse: the TOTAL SPEED LIMIT — PID on |v| — holds
        // in both directions, like the rollover protection which works on |v|.)

        if (m_cfg.open_loop != 0)
        {
            // OPEN-LOOP TEST MODE: the mixed stick goes straight to the motors — NO forward/
            // turn smoothing, NO rollover limit, NO speed-limiter PID, NO active (PID) braking,
            // and always the plain LINEAR mix (a diagnostic mode must not depend on the
            // feel curve selected in mix_type).
            // The output PWM cap (battery-voltage / manual duty) still bounds the drive, and
            // every safety gate (arming, heartbeat, e-stop, blocking faults) is unchanged
            // upstream. Releasing the stick engages DYNAMIC braking (motor short-circuit) — a
            // passive resting state, not a control loop — so it stops like every other mode
            // instead of coasting away.
            fwd = fwd_t;
            turn = turn_t;
            m_fwd_cmd = fwd;
            m_turn_cmd = turn;
            m_speed_pid.reset();
            if (std::fabs(fwd) < 1e-3f && std::fabs(turn) < 1e-3f)
            {
                // Released: dynamic brake (short-circuit) if enabled, else FREEWHEEL (coast).
                if (m_cfg.dyn_brake_en != 0) { braking = true; dyn_brake = true; }
            }
            else
            {
                mixArcade(fwd, turn, m_cfg.turn_gain, out_l, out_r);
                m_last_act_us = now;
            }
        }
        else
        {
            // Forward: NO input smoothing — the stick maps straight to the command. Any
            // acceleration limiting belongs in the control layer (speed PID / duty cap), not
            // here. Turn keeps its slope limiter (smooths abrupt steering, feeds rollover).
            m_fwd_cmd = fwd_t;
            m_turn_cmd = slew(turn_t, m_turn_cmd, m_cfg.turn_rate, hw::CTRL_DT_S);
            fwd = m_fwd_cmd;
            turn = m_turn_cmd;

            // "iso-a_lat" rollover protection: the turn limit follows the MEASURED speed
            // as 1/v (same lateral acceleration at all speeds — see turnLimit); ±100%
            // below turn_full_ms (pivot in place at full power), turn_hi at speed_limit_ms,
            // and even tighter beyond. Without encoders, v=0 → no limiting.
            // Disableable (turn_limit_en=0) for bench testing.
            if (m_cfg.turn_limit_en != 0)
            {
                const float turn_max = turnLimit(std::fabs(v_veh), m_cfg.turn_full_ms, m_cfg.speed_limit_ms, m_cfg.turn_hi);
                turn = clampf(turn, -turn_max, turn_max);
                m_turn_cmd = turn;   // keeps the state bounded (no ramp windup beyond the limit)
            }

            // Stick→motor mixing, pluggable (mixer.hpp): linear / expo / expo+speed-soft,
            // chosen from the web config. `turn` is already rollover-clamped above, and expo
            // only ever shrinks a magnitude, so no mixing type can widen that limit; the
            // speed limiter and PWM caps below apply to every type alike. v_veh (not v_meas):
            // without encoders the speed-adaptive type degrades to plain expo, like the
            // other speed-driven safeties.
            mixerFor(m_cfg.mix_type).mix(fwd, turn, v_veh, m_cfg, out_l, out_r);

            // Global speed cap (preserves the turn ratio) via PID on the average speed.
            // Target based on the MEASURED DIRECTION: forward → speed_limit_ms, reverse → rev_speed_ms. On the
            // measured direction (not the command): in plugging (reverse stick, kart still moving
            // forward) the target stays the forward one — the braking authority is not cut short.
            // Without encoders: no speed control loop → we rely on the PWM cap (duty_cap).
            if (use_enc && m_cfg.vlim_enable != 0)
            {
                const float v_target = (v_veh < 0.f) ? m_cfg.rev_speed_ms : m_cfg.speed_limit_ms;
                const float vcap = m_speed_pid.update(v_target, std::fabs(v_veh), hw::CTRL_DT_S,
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
                // ARMED + centered stick → ACTIVE BRAKING (plugging PID) if encoders present
                // AND PID braking enabled; else dynamic braking (short-circuit) if enabled;
                // else FREEWHEEL (coast). The disarmed/fault resting brake is separate (above).
                if (use_enc && m_cfg.brk_pid_enable != 0)
                {
                    braking = true;
                    out_l = brakeWheel(m_brake_l, sl, m_cfg, hw::CTRL_DT_S);
                    out_r = brakeWheel(m_brake_r, sr, m_cfg, hw::CTRL_DT_S);
                }
                else if (m_cfg.dyn_brake_en != 0)
                {
                    braking = true;
                    dyn_brake = true;
                }
                // else: freewheel — out stays 0, no braking.
            }
            else
            {
                m_last_act_us = now;
            }
        }
        state = State::Run;
    }

    out.dyn_brake = dyn_brake;   // motor short-circuit, otherwise driving / active plugging
    out.out_l = out_l;
    out.out_r = out_r;
    out.cap = cap;
    if (use_enc)
    {
        updateEncStuck(now, 0.5f * (out_l + out_r), v_veh);
        updateEncSanity(now, braking || dyn_brake, out_l, out_r, sl, sr);
    }

    // Auto disarm after inactivity.
    if (m_armed && (now - m_last_act_us) > static_cast<int64_t>(m_cfg.disarm_s) * 1000000) m_armed = false;

    // ── Tick telemetry ──
    m_tel.state      = state;
    m_tel.faults     = fmask;
    m_tel.vbat       = vbat;
    m_tel.batt_type  = m_batt_det.volts;
    // Don't publish a speed/rpm for a wheel whose encoder is IN ERROR — absent, magnet out of
    // field, or a latched encoder fault (stuck/reversed/erratic): the reading is meaningless →
    // 0 as fallback. (Control above still uses the RAW sl/sr so its sanity checks can fault.)
    const bool enc_faulted = m_enc_fault || m_enc_rev_fault || m_enc_mad_fault;
    const bool enc_l_valid = in.sensors.enc_ok_l && in.sensors.mag_ok_l && !enc_faulted;
    const bool enc_r_valid = in.sensors.enc_ok_r && in.sensors.mag_ok_r && !enc_faulted;
    const float sl_disp = enc_l_valid ? sl : 0.f;
    const float sr_disp = enc_r_valid ? sr : 0.f;
    m_tel.speed_l    = sl_disp;
    m_tel.speed_r    = sr_disp;
    m_tel.rpm_l      = enc_l_valid ? m_cps_l * m_cfg.enc_rpm_per_cps : 0.f;
    m_tel.rpm_r      = enc_r_valid ? m_cps_r * m_cfg.enc_rpm_per_cps : 0.f;
    m_tel.speed_ms   = 0.5f * (sl_disp + sr_disp);   // display; 0 for any wheel in error
    m_tel.fwd        = fwd;
    m_tel.turn       = turn;
    m_tel.out_l      = out_l;
    m_tel.out_r      = out_r;
    m_tel.brake_mode = dyn_brake ? BrakeMode::Dynamic : (braking ? BrakeMode::Active : BrakeMode::None);
    m_tel.armed      = m_armed;
    m_tel.btn_start  = in.btn_start_hw || in.pad.start;

    return out;
}

// ── Public THREAD-SAFE wrapper ─────────────────────────────────────────────
// The inputs/reads lock briefly; tick() runs step() under lock but
// calls the callbacks OUTSIDE the lock (no deadlock if the host reads the controller back).

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
    in.sensors = read ? read() : SensorReadings{};   // without wiring: "absent" sensors
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
