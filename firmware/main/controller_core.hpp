// controller_core.hpp — Kart control CORE: a PURE class (no ESP-IDF
// dependency, compilable on the host) that contains ALL the business logic to operate the kart.
// A host (real hardware or simulation) only has to:
//   1. fill the output callback (setCallbacks): apply the PWM/brake;
//   2. push the inputs when they change: setPad(), setConfig();
//   3. call tick(now_us) at the hw::CTRL_DT_S period.
// There is no sensor callback any more: nothing on the kart is measured (2026-09-29).
// The public API is THREAD-SAFE BY DEFAULT (internal mutex): the config can arrive from a
// web task while the control loop tick()s and a reader copies telemetry().
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "control_math.hpp"
#include "control_types.hpp"

// GAMEPAD state pushed by the host (setPad): percentages [-1..1] of the sticks and buttons.
// This is the LAST known state — tick() uses it as-is at each step.
struct PadInputs
{
    float x = 0.f;                   // CALIBRATED turn command (+ = right)
    float y = 0.f;                   // CALIBRATED forward command (+ = forward, − = reverse)
    float rx = 0.f;                  // RAW stick ("pushes but blocked" detection)
    float ry = 0.f;
    bool  connected = false;
    bool  calibrated = false;
    bool  estop = false;             // gamepad stop button (B)
    bool  start = false;             // gamepad START/Options button
    bool  drive = false;             // gamepad A button — HOLD TO DRIVE (dead man)
    int64_t last_report_us = 0;      // timestamp of the last HID report (heartbeat)
};

// ASSEMBLED inputs of a step — built by tick() from the pushed state and the clock,
// consumed by the business logic (step).
struct CtrlInputs
{
    int64_t now_us = 0;              // monotonic clock (µs)
    PadInputs pad;                   // gamepad (see setPad)
};

// ── Output of a tick: the MOTOR COMMAND, nothing else ──
// The core computes a motor output based on the inputs. Rumble, power
// cutoff, config persistence: HOST decisions derived from the telemetry
// (see advisors.hpp and the EspController/SimController hosts).
struct CtrlOutputs
{
    // EITHER the phase short-circuit (dyn_brake), OR the capped signed PWMs.
    bool     dyn_brake = true;       // default state: braking (never coasting)
    float    out_l = 0.f;            // driver channel L PWM [-1..1] (after mot_inv_*/mot_swap_lr)
    float    out_r = 0.f;            // driver channel R
    uint32_t cap = 0;                // duty cap (0..hw::PWM_MAX)
};

// Tick telemetry — coherent copy via telemetry() (published in g_status on the ESP side,
// inspected by the asserts in simulation).
struct CtrlTelemetry
{
    State     state = State::Lockout;
    Stop      stop = Stop::None;     // WHY the kart will not drive (one cause, see Stop) —
                                     // Stop::None while it drives or is merely disarmed
    float     fwd = 0.f;             // forward command after the deadzone [-1..1]
    float     turn = 0.f;            // turn command after the deadzone [-1..1]
    float     out_l = 0.f;           // logical per-WHEEL command (before mot_inv/mot_swap)
    float     out_r = 0.f;
    BrakeMode brake_mode = BrakeMode::Dynamic;
    bool      armed = false;
    bool      btn_start = false;     // gamepad START/Options held (display)
};

class KartController
{
public:
    using UpdateOutputsFn = std::function<void(const CtrlOutputs&)>;

    KartController() { m_cfg.setDefaults(); }

    // Wiring to the host world (once, before the loop): updateOutputs RECEIVES the outputs
    // of each step. Without wiring the outputs are only returned by tick().
    void setCallbacks(UpdateOutputsFn updateOutputs);

    // ── Inputs (thread-safe, to push when the info changes — the next tick uses it) ──
    void setConfig(const KartConfig& cfg);
    void setPad(const PadInputs& pad);

    // One control step (hw::CTRL_DT_S period): runs ALL the business logic under lock,
    // applies the outputs (callback) — and returns them.
    CtrlOutputs tick(int64_t now_us);

    // ── Reads (thread-safe: coherent copies under mutex) ──
    KartConfig    config() const;
    CtrlTelemetry telemetry() const;

private:
    CtrlOutputs step(const CtrlInputs& in);   // the business logic of a step (under lock)

    mutable std::mutex m_mtx;   // by-default thread-safety of the entire public API

    UpdateOutputsFn m_update_outputs;
    KartConfig m_cfg;                  // current configuration (see setConfig)
    PadInputs  m_pad;                  // last pushed gamepad state (see setPad)

    CtrlTelemetry m_tel;

    // ── Loop state ──
    bool    m_armed = false;
    int64_t m_hold_start_us = 0;   // START press start
    int64_t m_last_act_us = 0;     // last driving activity (A held + stick)
    bool    m_start_latch = false; // START press already handled
    // What the motors are CURRENTLY told, per wheel: the freewheel runs it down to 0, and
    // the accel ramp limits how fast it may grow. Both work on this one pair of values.
    float   m_last_l = 0.f;
    float   m_last_r = 0.f;
};
