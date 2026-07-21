// sim_controller.hpp — SIMULATED host for the controller: same wiring as EspController, but the
// KartController callbacks read the physics model (vehicle.hpp) and apply the motor
// command to it; the gamepad is SCRIPTED (function of time). Host decisions
// (rumble, cutoff) go through the SAME advisors as the ESP (advisors.hpp). Virtual
// clock: one tick = hw::CTRL_DT_S exactly. This is the "mockup" for the tests and the
// viewer.
#pragma once

#include <cstdint>
#include <functional>

#include "advisors.hpp"
#include "controller_core.hpp"
#include "vehicle.hpp"

namespace sim
{

// Command of the scripted gamepad at time t (simulation seconds).
struct PadCmd
{
    float x = 0.f;            // turn [-1..1] (already "calibrated": the script speaks in command)
    float y = 0.f;            // forward [-1..1]
    bool  start = false;      // START/Options button held
    bool  estop = false;      // B button
    bool  connected = true;
    bool  reports = true;     // false = link "connected" but NO more reports at all (heartbeat)
    bool  sys_power = true;   // false = KILL SWITCH: power cut off (ESP32 off,
                              // MOSFETs open → coasting) — the controller no longer runs
};
using PadScript = std::function<PadCmd(float t)>;

// Holding START during [t0, t0+dur] — the standard arming of the scenarios.
inline bool held(float t, float t0, float dur) { return t >= t0 && t <= t0 + dur; }

class SimController
{
public:
    SimController(Vehicle& veh, PadScript script)
        : m_veh(veh), m_script(std::move(script))
    {
        m_ctrl.setCallbacks([this] { return readSensors(); },
                            [this](const CtrlOutputs& out) { applyOutputs(out); });
    }

    // One full simulation step: gamepad → controller → host decisions → physics.
    void stepOnce(const KartConfig& cfg)
    {
        m_now_us += static_cast<int64_t>(hw::CTRL_DT_S * 1e6f);
        m_cmd = m_script(m_veh.t());
        if (!m_cmd.sys_power)
        {
            // Power cut off: the controller NO LONGER RUNS (no tick), the bridges
            // are open — the vehicle continues on its own, coasting.
            m_veh.step(DriveMode::Float, 0.f, 0.f, 0, hw::CTRL_DT_S);
            return;
        }
        if (m_cmd.connected && m_cmd.reports) m_last_report_us = m_now_us;

        PadInputs pad;
        pad.x = m_cmd.x;
        pad.y = m_cmd.y;
        pad.rx = m_cmd.x;   // the script speaks in command: raw = calibrated
        pad.ry = m_cmd.y;
        pad.connected = m_cmd.connected;
        pad.calibrated = calibrated;
        pad.estop = m_cmd.estop;
        pad.start = m_cmd.start;
        pad.last_report_us = m_last_report_us;

        m_vdiv = cfg.vbat_div_ratio;   // pin volts → battery volts conversion (readSensors)
        m_ctrl.setPad(pad);
        m_ctrl.setConfig(cfg);         // scenarios / driving mode change cfg on the fly
        m_ctrl.tick(m_now_us);

        // HOST decisions — same advisors as the ESP: rumble counted, cutoff remembered.
        const CtrlTelemetry t = m_ctrl.telemetry();
        if (m_rumble.update(t, pad, m_now_us).active) ++rumbles;
        powered_off |= m_poweroff.update(t, m_now_us);

        m_veh.step(m_brake_out ? DriveMode::Brake : DriveMode::Drive,
                   m_out_l, m_out_r, m_cap, hw::CTRL_DT_S);
    }

    CtrlTelemetry telemetry() const { return m_ctrl.telemetry(); }

    bool powered() const { return m_cmd.sys_power; }

    bool calibrated = true;    // calibration is a prerequisite, not the subject of the physics
    int  rumbles = 0;          // number of vibrations emitted (RumbleAdvisor)
    bool powered_off = false;  // cutoff requested (PowerOffAdvisor: prolonged LVC)

    // RAW gamepad input from the script (before deadzone/ramps/rollover protection)
    float padX() const { return m_cmd.x; }
    float padY() const { return m_cmd.y; }

    // Last motor command sent to the "hardware" (inspectable by the tests)
    float lastOutL() const { return m_out_l; }
    float lastOutR() const { return m_out_r; }
    bool  lastBrake() const { return m_brake_out; }

private:
    // Sensors callback — mirror of EspController::readSensors, the physics model in
    // place of the I2C bus: the failures (absent/reversed/stuck/crazy) come from the vehicle's EncMode.
    SensorReadings readSensors()
    {
        SensorReadings s;
        s.enc_delta_l = m_veh.encDelta(true);
        s.enc_delta_r = m_veh.encDelta(false);
        s.enc_ok_l = m_veh.encPresent(true);
        s.enc_ok_r = m_veh.encPresent(false);
        const float pin_v = m_veh.vbatPinVolts();
        s.vbat_ok = (pin_v >= 0.f);
        s.vbat_v = s.vbat_ok ? pin_v * m_vdiv : -1.f;
        return s;
    }

    // Outputs callback: stores the motor command (applied to the vehicle by stepOnce).
    void applyOutputs(const CtrlOutputs& out)
    {
        m_brake_out = out.dyn_brake;
        m_out_l = out.dyn_brake ? 0.f : out.out_l;
        m_out_r = out.dyn_brake ? 0.f : out.out_r;
        if (!out.dyn_brake) m_cap = out.cap;
    }

    KartController  m_ctrl;
    Vehicle&        m_veh;
    PadScript       m_script;
    PadCmd          m_cmd;
    RumbleAdvisor   m_rumble;
    PowerOffAdvisor m_poweroff;
    int64_t   m_now_us = 0;
    int64_t   m_last_report_us = 0;
    float     m_vdiv = 1.f;
    float     m_out_l = 0.f, m_out_r = 0.f;
    uint32_t  m_cap = hw::PWM_MAX;
    bool      m_brake_out = true;
};

} // namespace sim
