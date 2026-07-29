// controller_core.hpp — Kart control CORE: a PURE class (no ESP-IDF
// dependency, compilable on the host) that contains ALL the business logic to operate the kart.
// A host (real hardware or simulation) only has to:
//   1. fill the two callbacks (setCallbacks): updateSensors → read encoders/battery,
//      updateOutputs → apply the PWM/brake and the events (rumble, cutoff…);
//   2. push the inputs when they change: setPad(), setStartButton(), setConfig();
//   3. call tick(now_us) at the hw::CTRL_DT_S period.
// The public API is THREAD-SAFE BY DEFAULT (internal mutex): the config can arrive from a
// web task while the control loop tick()s and a reader copies telemetry().
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "control_math.hpp"
#include "control_types.hpp"
#include "pid.hpp"

// SENSOR reading (encoders + battery): data AND read errors — this is the
// return of the host's updateSensors callback (real I2C bus or physical model).
struct SensorReadings
{
    int   enc_delta_l = 0;    // AS5600 Δcounts since the last tick
    int   enc_delta_r = 0;
    bool  enc_ok_l = false;   // I2C read succeeded (false = sensor absent/silent)
    bool  enc_ok_r = false;
    bool  mag_ok_l = true;    // AS5600 STATUS: magnet properly in field (MD, not too weak/strong)
    bool  mag_ok_r = true;    // default true = assume OK if a host does not populate it
    float vbat_v = -1.f;      // BATTERY voltage in VOLTS — the ADC pin →
                              // battery conversion (divider ratio) is done ON THE HOST
    bool  vbat_ok = false;    // false = ADS1115 absent or read in error
    bool  motor_pwr = true;   // MOTOR power rail live (opto sense on the 40 A relay's output).
                              // Default true = assume live if a host does not populate it.
};

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
    int64_t last_report_us = 0;      // timestamp of the last HID report (heartbeat)
};

// ASSEMBLED inputs of a step — built by tick() from the pushed state + the sensor
// callback, consumed by the business logic (step).
struct CtrlInputs
{
    int64_t now_us = 0;              // monotonic clock (µs)
    PadInputs pad;                   // gamepad (see setPad)
    bool  btn_start_hw = false;      // physical START button (see setStartButton)
    SensorReadings sensors;          // encoders + battery voltage (see setCallbacks)
};

// ── Output of a tick: the MOTOR COMMAND, nothing else ──
// The core computes a motor output based on the inputs. Rumble, power
// cutoff, config persistence: HOST decisions derived from the telemetry
// (see advisors.hpp and the EspController/SimController hosts).
struct CtrlOutputs
{
    // EITHER the phase short-circuit (dyn_brake), OR the capped signed PWMs.
    bool     dyn_brake = true;       // default state: braking (never coasting)
    float    out_l = 0.f;            // left wheel PWM [-1..1]
    float    out_r = 0.f;
    uint32_t cap = 0;                // duty cap (0..hw::PWM_MAX)
};

// Tick telemetry — coherent copy via telemetry() (published in g_status on the ESP side,
// inspected by the asserts in simulation).
struct CtrlTelemetry
{
    State     state = State::Lockout;
    unsigned  faults = 0;            // BITSET of active errors/conditions (fb:: bits) —
                                     // the only representation of faults; the priority
                                     // display fault is derived via primaryFault()
    float     vbat = 0.f;
    int       batt_type = 0;         // 0 = detecting, 12 or 24
    float     speed_l = 0.f;         // SIGNED left wheel speed (m/s) — enc_mps_per_cps ratio
    float     speed_r = 0.f;
    float     rpm_l = 0.f;           // SIGNED left ENCODER-SHAFT rpm (raw) — enc_rpm_per_cps ratio
    float     rpm_r = 0.f;
    float     speed_ms = 0.f;        // SIGNED VEHICLE speed (m/s), 0 when pivoting
    float     fwd = 0.f;             // forward command after limits [-1..1]
    float     turn = 0.f;            // turn command after rollover protection [-1..1]
    float     out_l = 0.f;
    float     out_r = 0.f;
    BrakeMode brake_mode = BrakeMode::Dynamic;
    bool      armed = false;
    bool      btn_start = false;     // physical START OR gamepad (display)
};

class KartController
{
public:
    using UpdateSensorsFn = std::function<SensorReadings()>;
    using UpdateOutputsFn = std::function<void(const CtrlOutputs&)>;

    KartController() { m_cfg.setDefaults(); }

    // Wiring to the host world (once, before the loop): updateSensors is
    // CALLED by tick() to read the sensors; updateOutputs RECEIVES the outputs of the step.
    // Without wiring: "absent" sensors (SensorReadings{}), outputs lost.
    void setCallbacks(UpdateSensorsFn updateSensors, UpdateOutputsFn updateOutputs);

    // ── Inputs (thread-safe, to push when the info changes — the next tick uses it) ──
    void setConfig(const KartConfig& cfg);
    void setPad(const PadInputs& pad);
    void setStartButton(bool held);   // physical START button (debounce done by the host)

    // One control step (hw::CTRL_DT_S period): reads the sensors (callback), runs
    // ALL the business logic under lock, applies the outputs (callback) — and returns them.
    CtrlOutputs tick(int64_t now_us);

    // ── Reads (thread-safe: coherent copies under mutex) ──
    KartConfig    config() const;
    CtrlTelemetry telemetry() const;

private:
    CtrlOutputs step(const CtrlInputs& in);   // the business logic of a step (under lock)

    void  updateLVC(float vbat, int64_t now);
    void  updateEncStuck(int64_t now, float cmd, float speed_ms);
    void  updateEncSanity(int64_t now, bool braking, float out_l, float out_r, float sl, float sr);
    float brakeWheel(Pid& pid, float speed_ms, const KartConfig& cfg, float dt);

    mutable std::mutex m_mtx;   // by-default thread-safety of the entire public API

    UpdateSensorsFn m_update_sensors;
    UpdateOutputsFn m_update_outputs;
    KartConfig m_cfg;                  // current configuration (see setConfig)
    PadInputs  m_pad;                  // last pushed gamepad state (see setPad)
    bool       m_btn_start_hw = false; // last state of the physical START button

    CtrlTelemetry m_tel;

    // ── Loop state ──
    Pid     m_brake_l;             // left wheel brake (speed command 0, signed output)
    Pid     m_brake_r;
    Pid     m_speed_pid;           // global speed cap (output = fraction of PWM)
    bool    m_armed = false;
    bool    m_lvc_tripped = false;
    int64_t m_sag_start_us = 0;
    int64_t m_hold_start_us = 0;   // START press start
    int64_t m_last_act_us = 0;     // last stick activity
    bool    m_start_latch = false; // START press already handled
    int64_t m_stuck_us = 0;
    bool    m_enc_fault = false;
    ctl::RevDetect m_rev_l, m_rev_r;   // "reversed direction" per wheel
    int64_t m_mad_us = 0;              // start of persistent aberrant measurement
    bool    m_enc_rev_fault = false;   // encoder/motor wired backwards (latched)
    bool    m_enc_mad_fault = false;   // measurement without physical meaning (latched)
    float   m_fwd_cmd = 0.f;           // forward command (no smoothing — stick maps straight through)
    float   m_turn_cmd = 0.f;
    float   m_vbat_ema = 0.f;          // smoothed Vbat (τ ≈ 1 s) for the PWM cap — 0 = unknown
    float   m_vbat_lvc = 0.f;          // smoothed Vbat (τ = 2 s) the LVC judges — see hw::VBAT_LVC_EMA_*
    ctl::BattDetect m_batt_det;        // 12/24 V type classified at startup (voltage stable 3 s)
    float   m_cps_l = 0.f;             // SMOOTHED encoder count rate (EMA, counts/s)
    float   m_cps_r = 0.f;
    float   m_cps_win_l[5] = {0, 0, 0, 0, 0};   // 5-sample window for the spike-reject median
    float   m_cps_win_r[5] = {0, 0, 0, 0, 0};
    int     m_cps_win_i = 0;
    int64_t m_last_now_us = 0;                   // previous tick time → REAL dt for the count rate
    int     m_vbat_tick = 0;           // Vbat read rate-limit (hw::VBAT_READ_TICKS)
    float   m_vraw = -1.f;             // last raw reading (< 0 = sensor absent)
};
