// controller_core.cpp — Kart control logic (see controller_core.hpp).
// PURE: all I/O goes through the host (setPad/setConfig in, the output callback out).
// Arcade mixing: y = forward, x = turn → left = forward + turn, right = forward − turn.
// Safeties: gamepad disconnected/silent, gamepad e-stop, missing calibration → BRAKING.
// None of those is called a "fault" any more (2026-09-30): they are ordinary gamepad
// conditions, they disarm and brake, and the ONE that applies is published as Stop.
// (The HARDWARE emergency stop is not seen here: the mushroom opens the main relay's
// coil and the ESP dies with the rest — see doc/electronique.md.)
//
// SIMPLIFIED on 2026-09-29: no sensor on the wheels, so no speed limiter, no PID braking,
// no rollover protection and no turn smoothing. What is left, once armed:
//   A released           → dynamic brake (motor short-circuit), whatever the stick says;
//   A held + stick       → the mixed stick goes to the motors, bounded by duty_cap only;
//   A held + stick at 0  → PSEUDO-freewheel: the command SLIDES to 0 instead of dropping.
// Both directions are one rate limiter (control_math.hpp): accel_pct_s bounds the rise (for
// the motor drivers), decel_pct_s the fall — and that fall IS the freewheel, which is why the
// old coast_ms duration is gone. Neither delays the BRAKE: releasing A, disarming or any Stop
// cause grounds the windings on the tick it happens.
// ⚠️ Nothing limits the turn at speed any more: in simulation a full turn at full PWM tips
// the kart. duty_cap is the only lever (see its help text and the sim measurement).
#include "controller_core.hpp"

#include <algorithm>
#include <cmath>

#include "mixer.hpp"

namespace
{
using ctl::clampf;
using ctl::deadzone;
using ctl::ramp;
} // namespace

// step() — ALL the business logic of a step. Runs UNDER the lock taken by tick().
CtrlOutputs KartController::step(const CtrlInputs& in)
{
    CtrlOutputs out;
    const int64_t now = in.now_us;

    // Heartbeat: "connected" but no HID report for PAD_HB_TIMEOUT_US → treated as
    // DISCONNECTED (immediate disarm + braking, without waiting for the Bluetooth timeout).
    const bool pad_stale = in.pad.connected &&
                           ((now - in.pad.last_report_us) > hw::PAD_HB_TIMEOUT_US);

    // PWM cap: MANUAL only (duty_cap, web page) — the single ceiling on everything.
    const float    cap_frac = clampf(m_cfg.duty_cap_frac, 0.f, 1.f);
    const uint32_t cap = static_cast<uint32_t>(hw::PWM_MAX * cap_frac);

    // WHY the kart will not drive — ONE cause, highest priority first (see Stop). There is no
    // fault mask, no fault state and no fault LED any more: each of these conditions simply
    // disarms and brakes, and the cause is published so the page can say which.
    Stop stop = Stop::None;
    if      (!in.pad.connected)   stop = Stop::PadLost;
    else if (pad_stale)           stop = Stop::PadStale;
    else if (in.pad.estop)        stop = Stop::EStop;
    else if (!in.pad.calibrated)  stop = Stop::NotCalibrated;
    const bool blocked = (Stop::None != stop);

    // Anything in the way → disarm and brake. Re-arm (START held) once it is resolved.
    if (blocked) m_armed = false;

    // ── Arming by held press on START (anti-startup: stick centered + nothing in the way) ──
    // START = the gamepad's START/Options button, the ONLY arming input. Arming alone does
    // not move anything: the kart still needs A held (dead man) on top of the stick.
    const bool centered = (std::fabs(in.pad.x) < hw::ARM_CENTER_MAX) && (std::fabs(in.pad.y) < hw::ARM_CENTER_MAX);
    if (in.pad.start)
    {
        if (0 == m_hold_start_us) m_hold_start_us = now;
        else if (!m_start_latch && (now - m_hold_start_us) > static_cast<int64_t>(m_cfg.arm_hold_ms) * 1000)
        {
            m_start_latch = true;
            if (!m_armed && !blocked && centered)
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

    // m_armed is cleared above the moment anything blocks, so being armed IS being able to drive.
    const bool can_drive = m_armed;

    float out_l = 0.f, out_r = 0.f, fwd = 0.f, turn = 0.f;
    BrakeMode mode = BrakeMode::Dynamic;
    State state = State::Lockout;

    if (can_drive)
    {
        state = State::Run;
        if (in.pad.drive)
        {
            // The stick sets the TARGET; the ramp decides how fast the command may get there.
            // A centred stick simply targets 0 — that is the whole of the pseudo-freewheel.
            fwd  = deadzone(in.pad.y, m_cfg.thr_deadzone);
            turn = deadzone(in.pad.x, m_cfg.thr_deadzone);
            const bool stick_idle = (std::fabs(fwd) < 1e-3f) && (std::fabs(turn) < 1e-3f);
            float tgt_l = 0.f, tgt_r = 0.f;
            if (!stick_idle)
            {
                mixerFor(m_cfg.mix_type).mix(fwd, turn, m_cfg, tgt_l, tgt_r);
                m_last_act_us = now;
            }
            // Both rates are given in REAL duty per second, hence the division by the cap: at a
            // lower duty_cap the kart is slower, so the same real slope is a steeper one in
            // command units and it still pulls away — and stops — in the advertised time.
            const float inv_cap = 1.f / std::max(cap_frac, 0.05f);
            const float up = (static_cast<float>(m_cfg.accel_pct_s) / 100.f) * inv_cap;
            const float dn = (static_cast<float>(m_cfg.decel_pct_s) / 100.f) * inv_cap;
            m_last_l = ramp(tgt_l, m_last_l, up, dn, hw::CTRL_DT_S);
            m_last_r = ramp(tgt_r, m_last_r, up, dn, hw::CTRL_DT_S);
            out_l = m_last_l;
            out_r = m_last_r;

            const bool rolling = (std::fabs(out_l) > 1e-4f) || (std::fabs(out_r) > 1e-4f);
            if (!stick_idle)   mode = BrakeMode::None;    // the driver is asking for drive
            else if (rolling)  mode = BrakeMode::Coast;   // centred, still sliding down
            // Centred AND already at 0: the mode stays Dynamic below, and says so — on this
            // driver a command of 0 grounds both windings, so that is the brake, not a coast.
        }
    }
    if (BrakeMode::Dynamic == mode)
    {
        // Not armed / blocked / A released / run-down finished → DYNAMIC BRAKING.
        // Nothing to slide down from next time, and the accel ramp starts again from standstill.
        m_last_l = 0.f;
        m_last_r = 0.f;
    }

    out.dyn_brake = (BrakeMode::Dynamic == mode);
    // Motor SIGN (mot_inv_*, per WHEEL) then CHANNEL routing (mot_swap_lr), applied here and
    // only here: out_l/out_r stay the logical per-wheel "+ = forward" commands for the
    // telemetry; CtrlOutputs carries what each driver CHANNEL receives.
    const float wheel_l = (0 != m_cfg.mot_inv_l) ? -out_l : out_l;
    const float wheel_r = (0 != m_cfg.mot_inv_r) ? -out_r : out_r;
    const bool  swap    = (0 != m_cfg.mot_swap_lr);
    out.out_l = swap ? wheel_r : wheel_l;
    out.out_r = swap ? wheel_l : wheel_r;
    out.cap = cap;

    // Auto disarm after inactivity (no driving: A held + stick).
    if (m_armed && (now - m_last_act_us) > static_cast<int64_t>(m_cfg.disarm_s) * 1000000) m_armed = false;

    // ── Tick telemetry ──
    m_tel.state      = state;
    m_tel.stop       = stop;
    m_tel.fwd        = fwd;
    m_tel.turn       = turn;
    m_tel.out_l      = out_l;
    m_tel.out_r      = out_r;
    m_tel.brake_mode = mode;
    m_tel.armed      = m_armed;
    m_tel.btn_start  = in.pad.start;

    return out;
}

// ── Public THREAD-SAFE wrapper ─────────────────────────────────────────────
// The inputs/reads lock briefly; tick() runs step() under lock but
// calls the callbacks OUTSIDE the lock (no deadlock if the host reads the controller back).

void KartController::setCallbacks(UpdateOutputsFn updateOutputs)
{
    std::lock_guard<std::mutex> lk(m_mtx);
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

CtrlTelemetry KartController::telemetry() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_tel;
}

CtrlOutputs KartController::tick(int64_t now_us)
{
    UpdateOutputsFn apply;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        apply = m_update_outputs;
    }
    CtrlInputs in;
    in.now_us = now_us;
    CtrlOutputs out;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        in.pad = m_pad;
        out = step(in);
    }
    if (apply) apply(out);
    return out;
}
