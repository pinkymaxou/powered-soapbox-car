// test_main.cpp — HOST tests (g++, without ESP-IDF) of the firmware's pure logic:
// ring buffer (ringbuffer.hpp), control math (control_math.hpp) and mixers (mixer.hpp).
// Run: ./run_tests.sh (or see the CI "host-tests" job).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "control_math.hpp"
#include "mixer.hpp"
#include "ringbuffer.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// ───────────────────────── control_math ─────────────────────────
static void test_clampf()
{
    CHECK(near(ctl::clampf(0.5f, -1.f, 1.f), 0.5f));
    CHECK(near(ctl::clampf(2.f, -1.f, 1.f), 1.f));
    CHECK(near(ctl::clampf(-2.f, -1.f, 1.f), -1.f));
}

static void test_deadzone()
{
    const float dz = 0.06f;
    CHECK(near(ctl::deadzone(0.f, dz), 0.f));         // center → 0
    CHECK(near(ctl::deadzone(dz, dz), 0.f));          // deadzone edge → 0
    CHECK(near(ctl::deadzone(-dz, dz), 0.f));
    CHECK(near(ctl::deadzone(1.f, dz), 1.f));         // full travel → 1 (continuous remapping)
    CHECK(near(ctl::deadzone(-1.f, dz), -1.f));
    CHECK(ctl::deadzone(0.5f, dz) > 0.f);             // monotonic, sign preserved
    CHECK(ctl::deadzone(-0.5f, dz) < 0.f);
    CHECK(near(ctl::deadzone(0.5f, dz), -ctl::deadzone(-0.5f, dz)));   // symmetry
}

static void test_slew()
{
    // Step limited to rate·dt, in both directions; reaches the target without overshoot.
    CHECK(near(ctl::slew(1.f, 0.f, 2.f, 0.1f), 0.2f));
    CHECK(near(ctl::slew(-1.f, 0.f, 2.f, 0.1f), -0.2f));
    CHECK(near(ctl::slew(0.1f, 0.f, 2.f, 0.1f), 0.1f));   // target closer than the step → reached
    float v = 0.f;
    for (int i = 0; i < 100; ++i) v = ctl::slew(0.7f, v, 2.f, 0.01f);
    CHECK(near(v, 0.7f));
}

// ramp: the asymmetric limiter. `up` bounds growth away from zero, `dn` the fall back toward
// it; either at 0 lets that side through untouched. A reversal is dn then up, in that order.
static void test_ramp()
{
    CHECK(near(ctl::ramp(1.f, 0.f, 2.f, 4.f, 0.1f), 0.2f));      // growing → up·dt
    CHECK(near(ctl::ramp(-1.f, 0.f, 2.f, 4.f, 0.1f), -0.2f));    // …in both directions
    CHECK(near(ctl::ramp(0.f, 1.f, 2.f, 4.f, 0.1f), 0.6f));      // shrinking → dn·dt (faster)
    CHECK(near(ctl::ramp(0.3f, 1.f, 2.f, 4.f, 0.1f), 0.6f));     // toward a smaller target: dn
    CHECK(near(ctl::ramp(0.5f, 1.f, 2.f, 4.f, 0.2f), 0.5f));     // target inside the step → reached
    CHECK(near(ctl::ramp(1.f, 0.f, 0.f, 4.f, 0.1f), 1.f));       // up = 0 → no rise limit
    CHECK(near(ctl::ramp(0.f, 1.f, 2.f, 0.f, 0.1f), 0.f));       // dn = 0 → drops at once
    // Reversal: the fall to 0 is spent at dn, and only then does it build the other way at up.
    CHECK(near(ctl::ramp(-1.f, 0.5f, 2.f, 4.f, 0.1f), 0.1f));    // 0.5 → 0.1, still falling
    CHECK(near(ctl::ramp(-1.f, 0.f, 2.f, 4.f, 0.1f), -0.2f));    // …then up takes over
    // A tick that finishes the fall does NOT stall on 0: what is left of it already builds the
    // other way, so plugging never loses a tick sitting at zero.
    CHECK(near(ctl::ramp(-1.f, 0.1f, 2.f, 4.f, 0.1f), -0.2f));
    float v = 0.f;                                               // converges, no overshoot
    for (int i = 0; i < 100; ++i) v = ctl::ramp(0.7f, v, 2.f, 4.f, 0.01f);
    CHECK(near(v, 0.7f));
    for (int i = 0; i < 100; ++i) v = ctl::ramp(0.f, v, 2.f, 4.f, 0.01f);
    CHECK(near(v, 0.f));
}

static void test_mix_arcade()
{
    float l = 0.f, r = 0.f;
    ctl::mixArcade(1.f, 0.f, 0.6f, l, r);       // straight ahead → symmetric
    CHECK(near(l, 1.f) && near(r, 1.f));
    ctl::mixArcade(0.f, 1.f, 0.6f, l, r);       // stick right → pivot to the RIGHT
    CHECK(near(l, 0.6f) && near(r, -0.6f));     // (left accelerates, right reverses)
    ctl::mixArcade(0.f, -1.f, 0.6f, l, r);      // stick left → the RIGHT wheel accelerates
    CHECK(near(l, -0.6f) && near(r, 0.6f));
    ctl::mixArcade(1.f, 1.f, 0.6f, l, r);       // saturation clamped to ±1
    CHECK(near(l, 1.f) && near(r, 0.4f));
    ctl::mixArcade(-1.f, 0.f, 0.6f, l, r);      // reverse
    CHECK(near(l, -1.f) && near(r, -1.f));
}

static void test_square_map()
{
    float x, y;
    x = 1.f; y = 0.f; ctl::squareMap(x, y);                 // axes: unchanged
    CHECK(near(x, 1.f) && near(y, 0.f));
    x = 0.f; y = -1.f; ctl::squareMap(x, y);
    CHECK(near(x, 0.f) && near(y, -1.f));
    x = 0.f; y = 0.f; ctl::squareMap(x, y);                 // center: unchanged
    CHECK(near(x, 0.f) && near(y, 0.f));
    const float d = 0.70710678f;                            // full diagonal (on the circle)
    x = d; y = d; ctl::squareMap(x, y);                     // → corner of the square
    CHECK(near(x, 1.f, 1e-3f) && near(y, 1.f, 1e-3f));
    x = -d; y = d; ctl::squareMap(x, y);
    CHECK(near(x, -1.f, 1e-3f) && near(y, 1.f, 1e-3f));
    x = 0.3f; y = 0.4f; ctl::squareMap(x, y);               // direction preserved (x/y constant)
    CHECK(near(x / y, 0.75f, 1e-3f));
    CHECK(std::fabs(x) <= 1.f && std::fabs(y) <= 1.f);      // always bounded
}

// ───────────────────────── Ring ─────────────────────────
static void test_ring()
{
    Ring<uint8_t, 4> ring;
    CHECK(0 == ring.count() && 4 == ring.capacity());

    uint8_t lin[8] = {};
    CHECK(0 == ring.copyTo(lin, 8));                // empty → nothing

    ring.push(1); ring.push(2); ring.push(3);
    CHECK(3 == ring.count());
    CHECK(3 == ring.copyTo(lin, 8));
    CHECK(1 == lin[0] && 2 == lin[1] && 3 == lin[2]);

    ring.push(4); ring.push(5);   // overflow → overwrites the oldest
    CHECK(4 == ring.count());
    // Linearization (for the protobuf "bytes" encoding): oldest → newest.
    CHECK(4 == ring.copyTo(lin, 8));
    CHECK(2 == lin[0] && 3 == lin[1] && 4 == lin[2] && 5 == lin[3]);
    CHECK(2 == ring.copyTo(lin, 2));                // capacity < count → truncated to the oldest
    CHECK(2 == lin[0] && 3 == lin[1]);
}

// Pluggable stick→motor mixing (mixer.hpp): the expo curve's contract, then each mixer.
static void test_mixers()
{
    // The expo curve: endpoints exact, sign preserved, never AMPLIFIES, monotonic,
    // e=0 = identity.
    CHECK(near(mixdet::expo(1.f, 0.7f), 1.f));
    CHECK(near(mixdet::expo(-1.f, 0.7f), -1.f));
    CHECK(near(mixdet::expo(0.4f, 0.f), 0.4f));                       // e=0 → identity
    CHECK(near(mixdet::expo(0.5f, 1.f), 0.125f));                     // e=1 → pure cubic
    CHECK(near(mixdet::expo(0.5f, 0.5f), 0.3125f));                   // the blend
    CHECK(near(mixdet::expo(0.5f, 0.5f), -mixdet::expo(-0.5f, 0.5f)));// odd function
    for (float x = 0.f; x <= 1.001f; x += 0.05f)
    {
        CHECK(std::fabs(mixdet::expo(x, 0.6f)) <= x + 1e-6f);         // |expo| ≤ |x|
        if (x > 0.f) CHECK(mixdet::expo(x, 0.6f) > mixdet::expo(x - 0.05f, 0.6f));  // monotonic
    }

    KartConfig cfg{};
    cfg.turn_gain = 1.f;
    cfg.mix_expo_fwd = 0.5f;
    cfg.mix_expo_turn = 0.5f;
    float ll, lr, el, er;

    // Type 0 ≡ the historical mixArcade, and any unknown type (2 = the retired speed-soft
    // mixer, from an old NVS) falls back to it.
    mixerFor(0).mix(0.5f, 0.3f, cfg, ll, lr);
    float rl, rr;
    ctl::mixArcade(0.5f, 0.3f, cfg.turn_gain, rl, rr);
    CHECK(near(ll, rl) && near(lr, rr));
    for (int unknown : {2, 99})
    {
        mixerFor(unknown).mix(0.5f, 0.3f, cfg, el, er);
        CHECK(near(el, ll) && near(er, lr));
    }

    // EXPO: softer than linear at half stick, identical at full stick.
    mixerFor(1).mix(0.5f, 0.f, cfg, el, er);
    CHECK(near(el, 0.3125f) && near(er, 0.3125f));                    // 31% instead of 50%
    mixerFor(1).mix(1.f, 0.f, cfg, el, er);
    CHECK(near(el, 1.f) && near(er, 1.f));                            // stops keep full power
}

int main()
{
    test_clampf();
    test_deadzone();
    test_slew();
    test_ramp();
    test_mix_arcade();
    test_square_map();
    test_ring();
    test_mixers();

    if (0 == g_failures)
    {
        std::printf("All host tests PASS ✔\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
