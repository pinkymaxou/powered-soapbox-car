// mixer.hpp — Pluggable stick-to-motor MIXING (pure, header-only, host-compilable).
//
// One job: (forward, turn) → per-side motor commands. The kart proved hard for a child to
// drive with the plain linear mix — full sensitivity right from the stick center. The
// abstraction lets the web config choose the feel at runtime:
//
//   0 LINEAR — the historical arcade mix, unchanged. The reference.
//   1 EXPO   — classic RC exponential on both axes: gentle over a wide band around
//              center, full authority at the stops. expo(x) = (1-e)·x + e·x³.
//
// (Type 2, the speed-adaptive throttle, went with the encoders: nothing measures the speed.)
// The PWM cap applies DOWNSTREAM, to every type alike.
#pragma once

#include "control_math.hpp"
#include "control_types.hpp"

// Abstract mixer, selected per tick from the config (see mixerFor). Stateless by design:
// a mixer is a pure shape, so switching types mid-drive from the web page is glitchless.
class Mixer
{
public:
    virtual ~Mixer() = default;
    // fwd/turn in [-1..1], already deadzoned.
    virtual void mix(float fwd, float turn, const KartConfig& cfg,
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
    void mix(float fwd, float turn, const KartConfig& cfg,
             float& out_l, float& out_r) const override
    {
        ctl::mixArcade(fwd, turn, cfg.turn_gain, out_l, out_r);
    }
};

class MixerExpo : public Mixer
{
public:
    void mix(float fwd, float turn, const KartConfig& cfg,
             float& out_l, float& out_r) const override
    {
        ctl::mixArcade(mixdet::expo(fwd, cfg.mix_expo_fwd),
                       mixdet::expo(turn, cfg.mix_expo_turn), cfg.turn_gain, out_l, out_r);
    }
};

// Config value → mixer instance. Unknown value falls back to LINEAR: an out-of-range
// number from a future/older config must never leave the kart without a mixer.
inline const Mixer& mixerFor(int type)
{
    static const MixerLinear s_linear;
    static const MixerExpo   s_expo;
    switch (type)
    {
        case 1:  return s_expo;
        default: return s_linear;
    }
}
