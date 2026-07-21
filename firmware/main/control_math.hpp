// control_math.hpp — PURE control math (header-only, no ESP-IDF dependency).
// Extracted from controller.cpp / input.cpp to be shared AND testable on the host
// (see test_host/). Every function here must stay free of side effects.
#pragma once

#include <cmath>
#include <cstdint>

namespace ctl
{

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Deadzone with remapping: [dz..1] → [0..1] (continuous, symmetric).
inline float deadzone(float v, float dz)
{
    if (std::fabs(v) <= dz) return 0.f;
    const float s = (v > 0.f) ? 1.f : -1.f;
    return s * (std::fabs(v) - dz) / (1.f - dz);
}

// Slope limiter: brings `current` toward `target` by at most rate·dt.
inline float slew(float target, float current, float rate, float dt)
{
    const float step = rate * dt;
    const float diff = target - current;
    if (diff > step)  return current + step;
    if (diff < -step) return current - step;
    return target;
}

// Differential arcade mixing: left = forward + turn·gain, right = forward − turn·gain.
inline void mixArcade(float fwd, float turn, float gain, float& out_l, float& out_r)
{
    // Stick to the left (turn < 0) → the RIGHT wheel speeds up and the LEFT slows down (and
    // vice versa): the kart turns toward the stick side.
    out_l = clampf(fwd + turn * gain, -1.f, 1.f);
    out_r = clampf(fwd - turn * gain, -1.f, 1.f);
}

// "ISO-LATERAL-ACCELERATION" rollover protection: a_lat ≈ k·v·δ (δ = commanded
// differential) ⇒ the turn limit decreases as 1/v — the SAME lateral acceleration is
// allowed at all speeds, calibrated to equal hi_limit at v_max. Below v_full — and everywhere
// hi·v_max/v exceeds 100% (v ≤ hi·v_max) — full turn: pivot in place at full power stays
// allowed. Beyond v_max (runaway downhill), the limit KEEPS tightening: less authority = less
// risk. Replaces the old linear ramp: its excess at mid-speed tipped over offset loads as soon
// as turn_gain = 1 (proven by simulation, scenario adulte_enfant).
inline float turnLimit(float v_abs, float v_full, float v_max, float hi_limit)
{
    if (v_abs <= v_full) return 1.f;
    return clampf(hi_limit * v_max / std::max(v_abs, 0.05f), 0.f, 1.f);
}

// Automatic PWM cap based on the MEASURED battery voltage: the motors (v_nom, e.g. 12 V)
// must not see more than their nominal voltage on average → max duty = v_nom / vbat.
// 12 V battery → ~100%, 20 V → ~60%, 24 V → ~50%. Unknown voltage (sensor absent,
// vbat ≤ 0) or lower than v_nom → 1 (no automatic limiting).
inline float dutyCapVolts(float vbat, float v_nom)
{
    if (vbat <= v_nom) return 1.f;
    return v_nom / vbat;
}

// Detection of the battery TYPE at startup (12 V or 24 V, lead-acid): the voltage must
// stay STABLE (min-max spread ≤ tol) for stable_us, then classification by threshold —
// a 12 V even at full charge stays ≤ ~14.8 V, a 24 V even discharged stays ≥ ~21 V,
// so v24_min between the two decides unambiguously. We NEVER change battery with the
// system powered on: the result is FINAL until restart (0 = not yet classified).
struct BattDetect
{
    int64_t m_start_us = -1;   // start of the stability window (-1 = not started)
    float   m_min = 0.f, m_max = 0.f;
    int     volts = 0;         // 0 = unknown, then 12 or 24 (frozen)

    void update(float v, int64_t now_us, int64_t stable_us, float tol, float v24_min)
    {
        if (0 != volts) return;                                  // already classified: final
        if (v <= 0.f) { m_start_us = -1; return; }               // invalid reading → restart
        if (m_start_us < 0 || (v - m_min > tol) || (m_max - v > tol))
        {
            m_start_us = now_us;                                 // unstable → window restarted
            m_min = m_max = v;
            return;
        }
        m_min = (v < m_min) ? v : m_min;
        m_max = (v > m_max) ? v : m_max;
        if (now_us - m_start_us >= stable_us)
        {
            volts = (0.5f * (m_min + m_max) >= v24_min) ? 24 : 12;
        }
    }
};

// "Sensor/motor wired backwards" detection: firm command on one side, wheel measured
// FIRMLY on the other for win_us WITHOUT its speed decreasing. The key point:
// a commanded DECELERATION (braking at the stick, kart already moving) also has a speed opposed
// to the command — but it MELTS toward zero; reversed wiring gives an opposed speed that is
// STABLE or GROWING. We re-anchor the window whenever |v| decreases by more than `decay`.
struct RevDetect
{
    int64_t m_t0 = 0;    // start of the opposition window (0 = inactive)
    float   m_v0 = 0.f;  // |v| at the anchor

    // true = CONFIRMED reversal (firm, persistent and non-decreasing opposition).
    bool update(float out, float v, int64_t now_us, int64_t win_us,
                float out_min, float v_min, float decay)
    {
        const bool opposed = (std::fabs(out) > out_min) && (std::fabs(v) > v_min) &&
                             (out * v < 0.f);
        if (!opposed)
        {
            m_t0 = 0;
            return false;
        }
        if (0 == m_t0 || std::fabs(v) < m_v0 - decay)   // start, or it decelerates → re-anchor
        {
            m_t0 = now_us;
            m_v0 = std::fabs(v);
            return false;
        }
        return (now_us - m_t0) > win_us;
    }

    void reset() { m_t0 = 0; }
};

// Circle→square compensation: the physical stick is bounded by a CIRCLE (x²+y²≤1); at
// full diagonal each axis caps at ~0.71. Stretches radially (constant direction,
// factor |v|/max(|x|,|y|), =√2 at the diagonal) to make the corners of the SQUARE reachable.
inline void squareMap(float& x, float& y)
{
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float m = (ax > ay) ? ax : ay;
    if (m > 1e-3f)
    {
        const float scale = std::sqrt(x * x + y * y) / m;
        x = clampf(x * scale, -1.f, 1.f);
        y = clampf(y * scale, -1.f, 1.f);
    }
}

} // namespace ctl
