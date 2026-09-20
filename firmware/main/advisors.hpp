// advisors.hpp — HOST decisions derived from the core's telemetry (KartController).
// DELIBERATELY outside the core: the base controller computes a MOTOR output based
// on the inputs, nothing else. Vibrating the gamepad is presentation, so it belongs to the
// host — this PURE advisor (host-compilable) is shared by the ESP (EspController) and the
// simulation (SimController).
// The two power-cutoff advisors (prolonged LVC, idle timeout) were REMOVED with the power
// latch: the kart is switched on and off by its main switch and its e-stop, and the firmware
// has no way — and no business — cutting its own supply. See doc/electronique.md.
#pragma once

#include <cmath>
#include <cstdint>

#include "controller_core.hpp"

// Vibration command to send to the gamepad (input::rumble on the ESP side).
struct RumbleCmd
{
    bool     active = false;
    uint8_t  strong = 0;
    uint8_t  weak = 0;
    uint16_t duration_ms = 0;
};

// Haptic feedback on the EDGES of the telemetry: arming (soft), hard fault or
// e-stop (strong), "pushes the stick but blocked" (strong, repeated with anti-spam).
class RumbleAdvisor
{
public:
    RumbleCmd update(const CtrlTelemetry& t, const PadInputs& pad, int64_t now_us)
    {
        RumbleCmd cmd;
        auto set = [&cmd](uint8_t st, uint8_t wk, uint16_t ms) {
            cmd.active = true;
            cmd.strong = st;
            cmd.weak = wk;
            cmd.duration_ms = ms;
        };
        // "hard" fault: any encoder fault — they all force the stop
        // and deserve an emphatic haptic feedback.
        const bool hard_fault = (0 != (t.faults & fb::HARD));
        if (t.armed && !m_armed_prev)                                        // just armed → soft
            set(90, 160, 220);
        if ((hard_fault && !m_hard_prev) || (pad.estop && !m_estop_prev))    // sudden error / e-stop → strong
            set(255, 255, 450);
        const bool pushing = (std::fabs(pad.rx) > hw::PUSH_MIN) || (std::fabs(pad.ry) > hw::PUSH_MIN);
        const bool can_drive = (State::Run == t.state);   // Run ⟺ armed and no blocking condition
        if (pushing && !can_drive && (now_us - m_block_us) > hw::RUMBLE_BLOCK_INTERVAL_US)
        {
            set(220, 220, 250);   // moving the stick but blocked → strong (repeated)
            m_block_us = now_us;
        }
        m_armed_prev = t.armed;
        m_estop_prev = pad.estop;
        m_hard_prev = hard_fault;
        return cmd;
    }

private:
    bool    m_armed_prev = false;
    bool    m_estop_prev = false;
    bool    m_hard_prev = false;
    int64_t m_block_us = 0;
};
