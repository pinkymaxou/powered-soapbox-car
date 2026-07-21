// test_main.cpp — HOST tests (g++, without ESP-IDF) of the firmware's pure logic:
// PID (pid.hpp), ring buffer (ringbuffer.hpp) and control math (control_math.hpp).
// Run: ./run_tests.sh (or see the CI "host-tests" job).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "control_math.hpp"
#include "pid.hpp"
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

static void test_turn_limit()
{
    // Rollover protection ramp: ±100% below v_full, linear decrease down to hi at v_max.
    CHECK(near(ctl::turnLimit(0.0f, 0.5f, 3.3f, 0.5f), 1.f));    // pivot in place → 100%
    CHECK(near(ctl::turnLimit(0.5f, 0.5f, 3.3f, 0.5f), 1.f));    // at the threshold → still 100%
    CHECK(near(ctl::turnLimit(1.0f, 0.5f, 3.3f, 0.5f), 1.f));    // 1/v > 100% → still full
    CHECK(near(ctl::turnLimit(3.3f, 0.5f, 3.3f, 0.5f), 0.5f));   // at Vmax → 50%
    CHECK(near(ctl::turnLimit(9.0f, 0.5f, 3.3f, 0.5f), 0.5f * 3.3f / 9.f));   // runaway → tightens further (iso-a_lat)
    const float mid = ctl::turnLimit(1.9f, 0.5f, 3.3f, 0.5f);    // mid-speed → hi·vmax/v ≈ 87%
    CHECK(near(mid, 0.5f * 3.3f / 1.9f, 1e-3f));
    CHECK(near(ctl::turnLimit(2.0f, 0.5f, 0.5f, 0.5f), 0.125f)); // v_max ≤ v_full → 1/v direct, no division by 0
}

static void test_duty_cap_volts()
{
    // Auto PWM cap = V_nom / measured Vbat; 1 (no capping) if voltage unknown or low.
    CHECK(near(ctl::dutyCapVolts(0.f, 12.f), 1.f));      // sensor absent (Vbat unknown)
    CHECK(near(ctl::dutyCapVolts(-1.f, 12.f), 1.f));
    CHECK(near(ctl::dutyCapVolts(11.5f, 12.f), 1.f));    // discharged motorcycle battery → full duty
    CHECK(near(ctl::dutyCapVolts(12.f, 12.f), 1.f));     // exactly 12 V → 100%
    CHECK(near(ctl::dutyCapVolts(20.f, 12.f), 0.6f));    // 20 V tool pack → 60%
    CHECK(near(ctl::dutyCapVolts(24.f, 12.f), 0.5f));    // 24 V → 50%
    CHECK(near(ctl::dutyCapVolts(28.8f, 12.f), 12.f / 28.8f));   // 2×12 V charging
}

static void test_batt_detect()
{
    const int64_t S = 3000000;   // 3 s of stability required
    const float TOL = 0.5f, V24 = 18.f;
    const int64_t DT = 2000;     // 500 Hz

    // Stable 12 V battery → classified 12 after 3 s, not before.
    ctl::BattDetect d;
    int64_t t = 0;
    for (int i = 0; i < 1400; ++i) { d.update(12.6f, t, S, TOL, V24); t += DT; }
    CHECK(0 == d.volts);                     // 2.8 s: not yet
    for (int i = 0; i < 200; ++i) { d.update(12.6f, t, S, TOL, V24); t += DT; }
    CHECK(12 == d.volts);

    // 24 V battery (2×12 lead-acid, at rest ~25.3 V) → classified 24.
    ctl::BattDetect d24; t = 0;
    for (int i = 0; i < 1600; ++i) { d24.update(25.3f, t, S, TOL, V24); t += DT; }
    CHECK(24 == d24.volts);

    // UNSTABLE voltage (>|tol|) → the window restarts, no classification.
    ctl::BattDetect du; t = 0;
    for (int i = 0; i < 3000; ++i) { du.update((i % 2) ? 12.f : 13.f, t, S, TOL, V24); t += DT; }
    CHECK(0 == du.volts);
    // …then the voltage stabilizes → classified.
    for (int i = 0; i < 1600; ++i) { du.update(12.5f, t, S, TOL, V24); t += DT; }
    CHECK(12 == du.volts);

    // Invalid measurement (sensor absent) in the middle → restart from zero, without classifying.
    ctl::BattDetect di; t = 0;
    for (int i = 0; i < 1000; ++i) { di.update(12.6f, t, S, TOL, V24); t += DT; }
    di.update(-1.f, t, S, TOL, V24); t += DT;
    for (int i = 0; i < 1400; ++i) { di.update(12.6f, t, S, TOL, V24); t += DT; }
    CHECK(0 == di.volts);                    // 2.8 s since the dropout: not yet

    // Once classified, detection is FINAL (you don't swap batteries while powered on).
    ctl::BattDetect df; t = 0;
    for (int i = 0; i < 1600; ++i) { df.update(12.6f, t, S, TOL, V24); t += DT; }
    CHECK(12 == df.volts);
    for (int i = 0; i < 3000; ++i) { df.update(25.f, t, S, TOL, V24); t += DT; }
    CHECK(12 == df.volts);
}

static void test_rev_detect()
{
    const int64_t WIN = 400000;   // 400 ms
    const int64_t DT = 2000;      // tick 500 Hz
    const float OUT_MIN = 0.25f, V_MIN = 0.30f, DECAY = 0.15f;

    // STICK BRAKING: kart at +2 m/s, command -0.5 → the opposing speed DECAYS toward 0.
    // Deceleration 2 m/s²: NOT a reversal, no trigger expected.
    ctl::RevDetect d;
    int64_t t = 0; float v = 2.0f; bool fired = false;
    while (v > 0.f)
    {
        fired |= d.update(-0.5f, v, t, WIN, OUT_MIN, V_MIN, DECAY);
        v -= 2.0f * 0.002f;   // 2 m/s² × 2 ms
        t += DT;
    }
    CHECK(!fired);

    // REAL REVERSAL: command +0.5, measured wheel at -1 m/s STABLE → confirmed after 400 ms.
    ctl::RevDetect di; t = 0; fired = false;
    for (int i = 0; i < 300; ++i) { fired |= di.update(0.5f, -1.0f, t, WIN, OUT_MIN, V_MIN, DECAY); t += DT; }
    CHECK(fired);

    // Reversal with INCREASING speed (the kart accelerates, sensor backwards) → confirmed.
    ctl::RevDetect dc; t = 0; v = -0.4f; fired = false;
    for (int i = 0; i < 400; ++i)
    {
        fired |= dc.update(0.6f, v, t, WIN, OUT_MIN, V_MIN, DECAY);
        v -= 1.0f * 0.002f;   // accelerates in the wrong direction
        t += DT;
    }
    CHECK(fired);

    // Brief opposition (< 400 ms) then back to normal → nothing.
    ctl::RevDetect db; t = 0; fired = false;
    for (int i = 0; i < 100; ++i) { fired |= db.update(0.5f, -1.0f, t, WIN, OUT_MIN, V_MIN, DECAY); t += DT; }
    for (int i = 0; i < 300; ++i) { fired |= db.update(0.5f, 1.0f, t, WIN, OUT_MIN, V_MIN, DECAY); t += DT; }
    CHECK(!fired);
}

// ───────────────────────── Pid ─────────────────────────
static void test_pid()
{
    Pid pid;

    // Pure P: output proportional to the error, bounded.
    pid.reset();
    CHECK(near(pid.update(1.f, 0.f, 0.01f, 0.5f, 0.f, 0.f, -1.f, 1.f), 0.5f));
    CHECK(near(pid.update(10.f, 0.f, 0.01f, 0.5f, 0.f, 0.f, -1.f, 1.f), 1.f));   // clamp high

    // Integral: accumulates toward the command (stuck system → output rises).
    pid.reset();
    const float o1 = pid.update(1.f, 0.f, 0.1f, 0.f, 1.f, 0.f, -1.f, 1.f);
    const float o2 = pid.update(1.f, 0.f, 0.1f, 0.f, 1.f, 0.f, -1.f, 1.f);
    CHECK(o2 > o1);

    // Anti-windup: after a LONG saturation, an error reversal must lift off
    // immediately from the max (the integral was frozen, not inflated).
    pid.reset();
    for (int i = 0; i < 1000; ++i) pid.update(10.f, 0.f, 0.01f, 1.f, 1.f, 0.f, -1.f, 1.f);
    const float after = pid.update(0.f, 10.f, 0.01f, 1.f, 1.f, 0.f, -1.f, 1.f);
    CHECK(after < 1.f);   // a "wound-up" PID would stay stuck at +1

    // reset() properly clears the state.
    pid.reset();
    CHECK(near(pid.update(0.f, 0.f, 0.01f, 1.f, 1.f, 1.f, -1.f, 1.f), 0.f));
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

int main()
{
    test_clampf();
    test_deadzone();
    test_slew();
    test_mix_arcade();
    test_square_map();
    test_turn_limit();
    test_duty_cap_volts();
    test_batt_detect();
    test_rev_detect();
    test_pid();
    test_ring();

    if (0 == g_failures)
    {
        std::printf("All host tests PASS ✔\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
