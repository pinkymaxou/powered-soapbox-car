// pid.hpp — Reusable PID regulator with anti-windup (header-only).
#pragma once

#include <algorithm>

class Pid
{
public:
    void reset()
    {
        m_integ = 0;
        m_prev = 0;
    }

    // sp = setpoint, meas = measurement, dt = step (s), gains kp/ki/kd, output bounded [out_min..out_max].
    float update(float sp, float meas, float dt, float kp, float ki, float kd, float out_min, float out_max)
    {
        const float error = sp - meas;
        const float deriv = (dt > 0.f) ? (error - m_prev) / dt : 0.f;
        m_prev = error;

        // "candidate" integral (validated only if we do not sink further into saturation).
        const float integ_candidate = m_integ + error * dt;
        const float out_raw = kp * error + ki * integ_candidate + kd * deriv;
        const float out = std::clamp(out_raw, out_min, out_max);

        // Anti-windup (conditional integration): if the output saturates AND the error
        // pushes even further into saturation, we FREEZE the integral.
        const bool winding_up = (out_raw > out_max && error > 0.f) || (out_raw < out_min && error < 0.f);
        if (!winding_up)
        {
            m_integ = integ_candidate;
        }

        return out;
    }

private:
    float m_integ = 0;
    float m_prev = 0;
};
