// mixer.hpp — Pluggable stick-to-motor MIXING (pure, header-only, host-compilable).
//
// One job: (forward, turn, vehicle speed) → per-side motor commands. The kart proved hard
// for a child to drive with the plain linear mix — full sensitivity right from the stick
// center. The abstraction lets the web config choose the feel at runtime:
//
//   0 LINEAR — the historical arcade mix, unchanged. The reference.
//   1 EXPO   — classic RC exponential on both axes: gentle over a wide band around
//              center, full authority at the stops. expo(x) = (1-e)·x + e·x³.
//   2 SOFT   — EXPO plus speed-adaptive throttle: the faster the kart already goes, the
//              less aggressive the accelerating throttle becomes (down to mix_soft_hi at
//              the speed limit). BRAKING IS NEVER TAPERED: a command opposing the motion
//              (plugging, letting a runaway kart be caught) keeps full authority — going
//              gentle exactly when the child needs to stop would be backwards.
//
// What the mixers do NOT own, on purpose:
//   · The rollover protection (ctl::turnLimit) clamps `turn` UPSTREAM in step() — no
//     mixing choice can widen it. expo only ever REDUCES a magnitude (|expo(x)| ≤ |x|
//     for e in [0..1]), so the clamp survives every mixer here.
//   · The speed-limiter PID and the PWM caps apply DOWNSTREAM, unchanged.
//   · Without encoders v_ms is 0: SOFT degrades to EXPO (full authority), consistent
//     with the rollover limit being inert there too.
#pragma once

#include "control_math.hpp"
#include "control_types.hpp"

// Abstract mixer, selected per tick from the config (see mixerFor). Stateless by design:
// a mixer is a pure shape, so switching types mid-drive from the web page is glitchless.
class Mixer
{
public:
    virtual ~Mixer() = default;
    // fwd/turn in [-1..1] (deadzoned, slewed, rollover-clamped), v_ms SIGNED vehicle m/s.
    virtual void mix(float fwd, float turn, float v_ms, const KartConfig& cfg,
                     float& out_l, float& out_r) const = 0;
};

namespace mixdet
{
// RC-style exponential: blend linear ↔ cubic. e=0 → identity; e=1 → x³ (softest middle).
// Odd function (sign-preserving), endpoints ±1 exact, |expo(x)| ≤ |x| on [-1..1].
inline float expo(float x, float e)
{
    e = ctl::clampf(e, 0.f, 1.f);
    return (1.f - e) * x + e * x * x * x;
}
} // namespace mixdet

class MixerLinear : public Mixer
{
public:
    void mix(float fwd, float turn, float, const KartConfig& cfg,
             float& out_l, float& out_r) const override
    {
        ctl::mixArcade(fwd, turn, cfg.turn_gain, out_l, out_r);
    }
};

class MixerExpo : public Mixer
{
public:
    void mix(float fwd, float turn, float, const KartConfig& cfg,
             float& out_l, float& out_r) const override
    {
        ctl::mixArcade(mixdet::expo(fwd, cfg.mix_expo_fwd),
                       mixdet::expo(turn, cfg.mix_expo_turn), cfg.turn_gain, out_l, out_r);
    }
};

class MixerSoft : public Mixer
{
public:
    void mix(float fwd, float turn, float v_ms, const KartConfig& cfg,
             float& out_l, float& out_r) const override
    {
        float f = mixdet::expo(fwd, cfg.mix_expo_fwd);
        // Taper ONLY a command that pushes in the direction the kart already moves.
        // fwd·v < 0 is braking/plugging → full authority, always.
        if (f * v_ms > 0.f)
        {
            const float vref = (v_ms >= 0.f) ? cfg.speed_limit_ms : cfg.rev_speed_ms;
            const float t = ctl::clampf(std::fabs(v_ms) / std::max(vref, 0.1f), 0.f, 1.f);
            f *= 1.f - (1.f - ctl::clampf(cfg.mix_soft_hi, 0.f, 1.f)) * t;
        }
        ctl::mixArcade(f, mixdet::expo(turn, cfg.mix_expo_turn), cfg.turn_gain, out_l, out_r);
    }
};

// Config value → mixer instance. Unknown value falls back to LINEAR: an out-of-range
// number from a future/older config must never leave the kart without a mixer.
inline const Mixer& mixerFor(int type)
{
    static const MixerLinear s_linear;
    static const MixerExpo   s_expo;
    static const MixerSoft   s_soft;
    switch (type)
    {
        case 1:  return s_expo;
        case 2:  return s_soft;
        default: return s_linear;
    }
}
