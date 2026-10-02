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

// ASYMMETRIC rate limiter on a motor command: `up` bounds how fast it may grow AWAY from
// zero, `dn` how fast it may fall back TOWARD zero. Either rate ≤ 0 disables its own side
// (the command jumps straight to the target that way).
//   up  exists for the motor DRIVERS: a stick slammed open is a step from 0 to full duty, and
//       a brushed motor answers a step like that with its stall current.
//   dn  is what the freewheel run-down used to be, said as a rate instead of a duration —
//       the MDD20A cannot float its outputs, so "coasting" is the command sliding to 0.
// A REVERSAL is the two in sequence: fall to 0 at `dn`, then build the other way at `up`.
// Neither rate ever delays the real brake — releasing A, disarming or any Stop cause bypasses
// this entirely and grounds the windings on the spot.
inline float ramp(float target, float current, float up, float dn, float dt)
{
    if (target * current < 0.f)          // reversing: spend `dn` getting back to 0 first
    {
        if (dn > 0.f)
        {
            const float zeroed = slew(0.f, current, dn, dt);
            if (0.f != zeroed) return zeroed;
        }
        current = 0.f;
    }
    const float rate = (std::fabs(target) > std::fabs(current)) ? up : dn;
    if (rate <= 0.f) return target;      // that side disabled → straight through
    return slew(target, current, rate, dt);
}

// Differential arcade mixing: left = forward + turn·gain, right = forward − turn·gain.
inline void mixArcade(float fwd, float turn, float gain, float& out_l, float& out_r)
{
    // Stick to the left (turn < 0) → the RIGHT wheel speeds up and the LEFT slows down (and
    // vice versa): the kart turns toward the stick side.
    out_l = clampf(fwd + turn * gain, -1.f, 1.f);
    out_r = clampf(fwd - turn * gain, -1.f, 1.f);
}

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
