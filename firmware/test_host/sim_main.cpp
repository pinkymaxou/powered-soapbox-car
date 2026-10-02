// sim_main.cpp — Kart simulation: the REAL control logic (controller_core.cpp)
// drives a physics model (sim/vehicle.hpp) through extreme and realistic scenarios.
//
//   ./sim                       → all scenarios fast-forwarded + parameter sweep (CI)
//   ./sim --list                → list of scenarios (name + description)
//   ./sim --stream NAME         → one scenario, one JSON line per frame (60 Hz) on stdout
//   ./sim --stream NAME --realtime  → same, paced in real time (for the 3D viewer)
//   KART_SIM_TRACE=f.csv ./sim  → CSV trace of each test scenario (inspection)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "sim/scenarios.hpp"
#include "sim/terrain.hpp"

static int g_failures = 0;

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

using namespace sim;

namespace
{

// Optional CSV trace (KART_SIM_TRACE): one section per scenario, regenerable.
FILE* g_trace = nullptr;
void traceHook(const char* scen, const Vehicle& v, const SimController& c, const CtrlTelemetry& t)
{
    static int decim = 0;
    if (0 != (decim++ % 8)) return;   // ~60 Hz is enough for inspection
    std::fprintf(g_trace, "%s,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d\n",
                 scen, v.t(), v.v(), v.yawRate(), v.aLat(), v.tipMargin(),
                 c.lastOutL(), c.lastOutR(), t.fwd, t.turn,
                 static_cast<int>(t.state), static_cast<int>(t.stop),
                 static_cast<int>(t.brake_mode));
}

RunResult run(const Scenario& sc)
{
    if (!g_trace) return runScenario(sc);
    return runScenario(sc, [&](const Vehicle& v, const SimController& c, const CtrlTelemetry& t) {
        traceHook(sc.name.c_str(), v, c, t);
    });
}

// ─────────────────────────── Tests (asserts) ───────────────────────────
// Speed of the kart at simulation time `at` in a scenario (via the frame hook).
float speedAt(const Scenario& sc, float at)
{
    float v_at = 0.f;
    runScenario(sc, [&](const Vehicle& v, const SimController&, const CtrlTelemetry&) {
        if (v.t() <= at) v_at = v.v();
    });
    return v_at;
}

Scenario withCap(Scenario sc, float cap)
{
    sc.cfg = [cap](KartConfig& c) { c.duty_cap_frac = cap; };
    return sc;
}

// The core's OUTPUT contract, tick by tick, without the physics: what reaches the driver for
// each A / stick combination, the accel ramp, the freewheel run-down pace (and both of their
// "disabled" settings), and the mot_inv/mot_swap routing.
void testCoreOutputs()
{
    KartController k;
    KartConfig cfg;
    cfg.setDefaults();
    k.setConfig(cfg);
    int64_t now = 0;
    PadInputs p;
    p.connected = true;
    p.calibrated = true;
    auto tick = [&](int n) {
        CtrlOutputs o;
        for (int i = 0; i < n; ++i) { now += 2000; p.last_report_us = now; k.setPad(p); o = k.tick(now); }
        return o;
    };
    p.start = true;
    tick(static_cast<int>(cfg.arm_hold_ms / 2 + 10));   // START held past arm_hold_ms
    p.start = false;
    CHECK(tick(1).dyn_brake);
    CHECK(k.telemetry().armed);

    p.y = 1.f;                                  // stick pushed, A released → brake
    CHECK(tick(5).dyn_brake);

    // ACCEL RAMP (accel_pct_s = 200 %/s by default): full stick does NOT reach the driver at
    // once — the command climbs at 2.0 per second, so half a second to go from 0 to full.
    p.drive = true;
    CtrlOutputs o = tick(1);
    CHECK(!o.dyn_brake && near(o.out_l, 0.004f, 1e-3f));   // one 2 ms tick of ramp
    CHECK(BrakeMode::None == k.telemetry().brake_mode);
    o = tick(125);                              // 0.25 s later: half way up
    CHECK(near(o.out_l, 0.5f, 5e-3f) && near(o.out_r, 0.5f, 5e-3f));
    o = tick(125);                              // 0.5 s: full command, and it stops there
    CHECK(near(o.out_l, 1.f) && near(o.out_r, 1.f));

    // Pulled back to half stick, the command comes DOWN at the decel rate (100 %/s → 0.2 per
    // 0.2 s), not instantly: one tick barely moves it. The real brake never waits for this,
    // though — that is A, tested below.
    p.y = 0.5f;                                 // (0.5 through the deadzone remap ≈ 0.47)
    o = tick(1);
    CHECK(o.out_l > 0.99f);                     // one 2 ms tick of decel: still essentially full
    o = tick(250);                              // half a second later it has reached the target
    CHECK(o.out_l < 0.5f && o.out_l > 0.4f);
    p.y = 1.f;

    // FREEWHEEL (decel_pct_s = 100 %/s by default): the stick centred simply targets 0, and
    // the command SLIDES there at the decel rate — half a second leaves half of it.
    o = tick(250);                              // back to full first (accel ramp)
    p.y = 0.f;                                  // stick released, A held → pseudo-freewheel
    o = tick(250);                              // 0.5 s at cap 1.0, 100 %/s → 0.5 left
    CHECK(!o.dyn_brake);
    CHECK(near(o.out_l, 0.5f, 5e-3f) && near(o.out_r, 0.5f, 5e-3f));
    CHECK(BrakeMode::Coast == k.telemetry().brake_mode);
    o = tick(static_cast<int>(1.f * hw::CTRL_HZ));   // the rest of the slide
    CHECK(near(o.out_l, 0.f) && near(o.out_r, 0.f));
    // Arrived at 0 with the stick still centred, the mode tells the truth: a command of 0 IS
    // the short-circuit on this driver, so it reports DYNAMIC, not a coast that is over.
    CHECK(o.dyn_brake && BrakeMode::Dynamic == k.telemetry().brake_mode);

    // At cap 0.5 the pace in COMMAND units doubles (both rates are set in REAL duty): a quarter
    // of a second of slide is now what half a second was.
    cfg.duty_cap_frac = 0.5f; cfg.accel_pct_s = 0; k.setConfig(cfg);   // accel off: full at once
    p.y = 1.f; o = tick(1);
    CHECK(near(o.out_l, 1.f));                  // accel_pct_s = 0 → no rise limit at all
    p.y = 0.f;
    o = tick(125);
    CHECK(near(o.out_l, 0.5f, 5e-3f));
    p.drive = false;                            // A released mid-slide → brake at once
    CHECK(tick(1).dyn_brake);
    p.drive = true;                             // …and the next slide restarts from nothing
    o = tick(1);
    CHECK(near(o.out_l, 0.f));

    // decel_pct_s = 0 → no fall limit: releasing the stick with A held brakes on the spot.
    cfg.duty_cap_frac = 1.f; cfg.accel_pct_s = 200; cfg.decel_pct_s = 0; k.setConfig(cfg);
    p.y = 1.f; tick(250);
    p.y = 0.f;
    CHECK(tick(1).dyn_brake);
    CHECK(BrakeMode::Dynamic == k.telemetry().brake_mode);

    cfg.decel_pct_s = 100; cfg.accel_pct_s = 0;   // routing: sign per WHEEL, then the channel swap
    cfg.mot_inv_l = 1; cfg.mot_swap_lr = 1; k.setConfig(cfg);
    p.y = 1.f; p.x = 1.f;                       // logical: left 1.0 (clamped), right 0.0
    o = tick(1);
    CHECK(near(k.telemetry().out_l, 1.f) && near(k.telemetry().out_r, 0.f));
    CHECK(near(o.out_l, 0.f) && near(o.out_r, -1.f));   // inverted left wheel, on channel R

    p.estop = true;                             // B → disarm + brake
    CHECK(tick(1).dyn_brake);
    CHECK(!k.telemetry().armed);
    CHECK(Stop::EStop == k.telemetry().stop);
    std::printf("  core outputs : A released=brake, accel/decel ramps, A+centre=freewheel, routing OK\n");
}

void testScenarios()
{
    const auto all = allScenarios();
    auto get = [&](const char* n) -> const Scenario& {
        const Scenario* s = findScenario(all, n);
        if (!s) { std::printf("scenario not found: %s\n", n); std::exit(2); }
        return *s;
    };

    // ⚠️ NO ROLLOVER PROTECTION (2026-09-29, the owner's call — no encoders, so nothing
    // measures the speed a turn limit would need). These are MEASUREMENTS pinned as asserts,
    // so that the README's warning can never drift from what the model says. With the slower
    // 18T→32T chain stage (1:23.70), two children sitting centred stay planted through a
    // full-speed full turn at duty_cap 1.0 — it is the OFF-CENTRE loads below that tip.
    {
        const RunResult r = run(get("virage_pleine_vitesse"));
        std::printf("  virage_pleine_vitesse (duty_cap 1.0, no protection) : vmax=%.2f m/s  "
                    "min margin=%.2f m/s² — %s\n", r.max_v, r.min_tip_margin,
                    r.tipped ? "TIPPED" : (r.wheel_lifted ? "wheel lifted" : "planted"));
        CHECK(r.ever_armed);
        CHECK(r.max_v > 2.0f);
        CHECK(!r.wheel_lifted && !r.tipped);
    }
    {
        const RunResult r = runScenario(withCap(get("virage_pleine_vitesse"), 0.6f));
        std::printf("  virage_pleine_vitesse (duty_cap 0.6) : vmax=%.2f m/s  min margin=%.2f m/s²\n",
                    r.max_v, r.min_tip_margin);
        CHECK(!r.wheel_lifted && !r.tipped);
        CHECK(r.min_tip_margin > 1.f);
    }
    // The old layout (mass far from the driven axle), with the faster gearing it was measured
    // with: it tips. The record of why the tricycle was reversed, and it keeps the tip
    // measurement honest (a model that never tips would prove nothing).
    {
        const RunResult r = run(get("ancienne_disposition"));
        std::printf("  ancienne_disposition : min margin=%.2f m/s² — %s\n",
                    r.min_tip_margin, r.tipped ? "TIPPED" : "?");
        CHECK(r.tipped);
    }
    // What the un-built two-caster variant would have bought, measured on the same run.
    {
        Scenario sc = get("virage_pleine_vitesse");
        sc.veh = [](Vehicle& veh) { veh.params().wheels = 4; };
        std::printf("  …the 4-wheel variant would give : %.2f m/s² (not built)\n",
                    runScenario(sc).min_tip_margin);
    }

    // Pivot in place: legal and stable (near-zero speed → near-zero a_lat).
    {
        const RunResult r = run(get("pivot_surplace"));
        CHECK(r.ever_armed);
        CHECK(std::fabs(r.max_v) < 0.6f);
        CHECK(r.min_tip_margin > 2.f);
        CHECK(!r.ever_blocked);
    }

    // Realistic driving, without the protection: measured and printed. They must at least
    // stay on their wheels — these are the manoeuvres a child actually makes.
    for (const char* n : {"slalom", "conduite_enfant"})
    {
        const RunResult r = run(get(n));
        std::printf("  %s : min margin=%.2f m/s²%s\n", n, r.min_tip_margin,
                    r.wheel_lifted ? " — WHEEL LIFTED" : "");
        CHECK(r.ever_armed && !r.ever_blocked);
        CHECK(!r.tipped);
    }

    // Stick braking (plugging, A held): nothing in the way, and it really went fast first.
    {
        const RunResult r = run(get("freinage_stick"));
        CHECK(r.ever_armed);
        CHECK(!r.ever_blocked);
        CHECK(r.max_v > 2.0f);
    }
    // Reverse: nothing holds it back but duty_cap — as fast as forward.
    {
        const RunResult r = run(get("marche_arriere"));
        CHECK(r.ever_armed && !r.ever_blocked);
        CHECK(r.final_v < -2.f);   // ≈ the forward top speed
        std::printf("  marche_arriere : final v=%.2f m/s (no reverse limit any more)\n", r.final_v);
    }
    // Expo mixing must not cost the top end.
    {
        const RunResult r = run(get("mix_expo"));
        CHECK(r.ever_armed && !r.ever_blocked);
        CHECK(r.max_v > 2.0f);              // expo(±1) = ±1: full stick keeps the top end
    }

    // ── The A button ──
    // Armed, full stick, A never held: the kart does not move — and the pad does NOT buzz
    // (only "not armed" buzzes; A released is the normal way to brake).
    {
        const RunResult r = run(get("sans_bouton_A"));
        CHECK(r.ever_armed);
        CHECK(r.max_v < 0.05f);
        CHECK(1 == r.rumbles);              // the soft arming buzz, nothing else
    }
    // A released at full speed: dynamic brake — well slowed 1 s later, stopped at the end.
    // A held with the stick released: pseudo-freewheel — still rolling clearly faster half a
    // second in, and down to a stop on its own. Sampled at HALF a second, because at the
    // default decel of 100 %/s the slide is over one second after the stick is centred: any
    // later and this would compare two brakes rather than a brake against a freewheel.
    {
        const float v_brake = speedAt(get("relache_A"), 5.5f);     // released at 5 s
        const float v_coast = speedAt(get("roue_libre_A"), 6.5f);  // released at 6 s
        KartConfig dflt; dflt.setDefaults();
        const RunResult rb = run(get("relache_A"));
        const RunResult rc = run(get("roue_libre_A"));
        std::printf("  relache_A : v=%.2f m/s 0.5 s after releasing A (dynamic brake)\n", v_brake);
        std::printf("  roue_libre_A : v=%.2f m/s 0.5 s after releasing the stick (decel "
                    "%d %%/s), final %.2f\n", v_coast, dflt.decel_pct_s, rc.final_v);
        CHECK(rb.ever_armed && !rb.ever_blocked);
        CHECK(v_brake < 1.8f);                    // the dynamic brake bit, hard, from 2.5 m/s
        CHECK(std::fabs(rb.final_v) < 0.2f);
        CHECK(rc.ever_armed && !rc.ever_blocked);
        CHECK(v_coast > v_brake + 0.2f);          // …and the run-down let it roll on further
        CHECK(std::fabs(rc.final_v) < 0.1f);      // both end stopped: at 0 the run-down IS the brake
    }

    // Heartbeat: loss of reports at full speed → disarmed quickly, kart stops.
    {
        const RunResult r = run(get("heartbeat_perte"));
        CHECK(r.ever_armed);
        // Reports cut at t=5 s; disarm within PAD_HB_TIMEOUT_US (750 ms) + margin.
        CHECK(r.t_disarmed_after >= 5.f && r.t_disarmed_after < 5.f + hw::PAD_HB_TIMEOUT_US * 1e-6f + 0.1f);
        CHECK(std::fabs(r.final_v) < 0.3f);                               // braked (dynamic)
        CHECK(Stop::PadStale == r.final_stop);   // …and the kart can SAY why it stopped
    }
    // The emergency stop as it now behaves: the mushroom opens the main relay's coil, so the
    // ESP32 dies with the motors. Nothing is reported (nobody is left to report it) and
    // NOTHING brakes — the kart coasts. That is the honest measurement, printed here.
    {
        const RunResult r = run(get("arret_urgence_plat"));
        CHECK(r.ever_armed);
        CHECK(!r.ever_blocked);                       // the firmware never sees the e-stop
        CHECK(std::fabs(r.final_v) < 0.05f);        // it does stop — eventually, on rolling drag
        CHECK(r.coast_dist > 10.f);                 // …after more than ten metres of coasting
        std::printf("  arret_urgence_plat : e-stop at full speed → coasts %.1f m before stopping "
                    "(no braking left at all)\n", r.coast_dist);
    }
    // A worn pack is not reported anywhere — it is just less voltage at the motors.
    {
        const RunResult r = run(get("batterie_usee"));
        CHECK(!r.ever_blocked);
        CHECK(Stop::None == r.final_stop);
        CHECK(r.max_v > 1.0f);
        std::printf("  batterie_usee : nothing reported (nothing measures it), max v=%.2f m/s\n", r.max_v);
    }

    // ASYMMETRIC LOADS, full-speed turns both ways: at duty_cap 1.0 both TIP — the reason the
    // duty_cap help text names 0.9 as the ceiling for these loads. Checked below at 0.6.
    for (const char* n : {"enfant_seul_cote", "adulte_enfant"})
    {
        const RunResult r = run(get(n));
        const RunResult r6 = runScenario(withCap(get(n), 0.6f));
        std::printf("  %s : duty_cap 1.0 → %s (%.2f) · duty_cap 0.6 → margin %.2f m/s²\n", n,
                    r.tipped ? "TIPPED" : (r.wheel_lifted ? "wheel lifted" : "planted"),
                    r.min_tip_margin, r6.min_tip_margin);
        CHECK(r.tipped);
        CHECK(!r6.wheel_lifted && !r6.tipped);
        CHECK(r6.min_tip_margin > 1.f);
    }

    // POWER CUT ON A SLOPE (main relay open → coasting): with no power there is NO electric
    // braking left AT ALL, and the kart RUNS AWAY down the slope. The E-STOP opens that same
    // relay, so this is also what pressing the mushroom on a hill costs.
    {
        const RunResult r = run(get("coupure_pente8"));
        std::printf("  coupure_pente8 : v(+8 s after the power cut)=%.1f m/s — RUNAWAY\n",
                    std::fabs(r.final_v));
        CHECK(std::fabs(r.final_v) > 3.f);
    }
    {
        const RunResult r = run(get("coupure_pente16"));
        std::printf("  coupure_pente16 : v(+8 s after the power cut)=%.1f m/s — RUNAWAY\n",
                    std::fabs(r.final_v));
        CHECK(std::fabs(r.final_v) > 8.f);
    }
    // A released on a slope → dynamic braking only: it cannot stop (force ∝ v) but caps the
    // descent. The 18T→32T stage gives ×1.39 of wheel torque, which brought the 16 % slope
    // within reach: it used to run away at 3.6 m/s (1:17.07), it now creeps at ~0.5.
    {
        const RunResult r = run(get("descente_frein_dynamique"));
        std::printf("  descente_frein_dynamique (8 %%, A released) : final v=%.2f m/s\n",
                    std::fabs(r.final_v));
        CHECK(r.ever_armed && !r.ever_blocked);
        CHECK(std::fabs(r.final_v) < 0.7f);   // crawling terminal speed ("it holds")
        CHECK(std::fabs(r.final_v) > 0.05f);  // …but does NOT STOP: documented limit
    }
    {
        const RunResult r = run(get("descente16_frein_dynamique"));
        std::printf("  descente16_frein_dynamique (16 %%, A released) : final v=%.2f m/s\n",
                    std::fabs(r.final_v));
        CHECK(r.ever_armed && !r.ever_blocked);
        CHECK(std::fabs(r.final_v) < 1.f);    // creeps down…
        CHECK(std::fabs(r.final_v) > 0.1f);   // …but does not stop
    }
}

// duty_cap sweep over the three hard load cases: the ONLY safety lever left, so its table is
// printed at every run and the ceiling quoted in the duty_cap help text (0.9) is pinned.
// Full throttle in a straight line then a full turn is the forgiving case; the off-centre
// child and the adult aboard are what set the number.
void testCapSweep()
{
    static const char* CASES[] = {"virage_pleine_vitesse", "enfant_seul_cote", "adulte_enfant"};
    const auto all = allScenarios();
    std::printf("  duty_cap sweep — min tip margin (m/s², < 0 = tips):\n      cap");
    for (const char* n : CASES) std::printf("  %22s", n);
    std::printf("\n");
    for (float cap : {1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f})
    {
        std::printf("     %.1f ", cap);
        for (const char* n : CASES)
        {
            const RunResult r = runScenario(withCap(*findScenario(all, n), cap));
            std::printf("  %15.2f %-6s", r.min_tip_margin, r.tipped ? "TIPS" : r.wheel_lifted ? "lifts" : "");
            if (cap <= 0.9f + 1e-3f)
            {
                CHECK(!r.wheel_lifted && !r.tipped);
                CHECK(r.min_tip_margin > 0.3f);
            }
        }
        std::printf("\n");
    }
}

// ─────────────────────────── JSON stream (viewer) ───────────────────────────
// One frame of the stream (shared between scripted scenarios and manual driving).
void printFrame(const Vehicle& v, const SimController& c, const CtrlTelemetry& t,
                bool backrooms = false)
{
    const float mps2rpm = 60.f / (2.f * PI_F * v.params().wheel_r_m);   // wheel rpm
    std::printf("{\"t\":%.3f,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"h\":%.4f,\"v\":%.3f,\"w\":%.3f,"
                "\"alat\":%.3f,\"margin\":%.3f,\"roll\":%.4f,\"pitch\":%.4f,\"lift\":%d,\"tipped\":%s,\"air\":%s,"
                "\"outl\":%.3f,\"outr\":%.3f,"
                "\"rpml\":%.2f,\"rpmr\":%.2f,"
                "\"p_w\":%.1f,\"e_wh\":%.3f,"
                "\"stickx\":%.2f,\"sticky\":%.2f,"
                "\"padx\":%.2f,\"pady\":%.2f,\"state\":%d,\"stop\":%d,"
                "\"brake\":%d,\"armed\":%s,\"power\":%s,\"backrooms\":%s}\n",
                v.t(), v.x(), v.y(), v.z(), v.heading(), v.v(), v.yawRate(),
                v.aLat(), v.tipMargin(), v.roll(), v.pitch(), v.liftSide(),
                v.tipped() ? "true" : "false", v.airborne() ? "true" : "false",
                c.lastOutL(), c.lastOutR(),
                v.wheelV(true) * mps2rpm, v.wheelV(false) * mps2rpm,
                v.powerW(), v.energyWh(),
                c.padX(), c.padY(),
                t.turn, t.fwd, static_cast<int>(t.state), static_cast<int>(t.stop),
                static_cast<int>(t.brake_mode), t.armed ? "true" : "false",
                c.powered() ? "true" : "false", backrooms ? "true" : "false");
    std::fflush(stdout);
}

int streamScenario(const std::string& name, bool realtime)
{
    const auto all = allScenarios();
    const Scenario* sc = findScenario(all, name);
    if (!sc)
    {
        std::fprintf(stderr, "unknown scenario: %s (see --list)\n", name.c_str());
        return 2;
    }
    // The scenario's vehicle (same mutations as in runScenario) → its real parameters
    // go into the meta message: the viewer's "Assumptions" button displays them
    // from THE source of truth, not from a copy.
    Vehicle vmeta;
    if (sc->veh) sc->veh(vmeta);
    const VehicleParams& p = vmeta.params();
    std::printf("{\"meta\":true,\"name\":\"%s\",\"desc\":\"%s\",\"duration\":%.1f,"
                "\"atip\":%.3f,\"cap\":%.2f,\"params\":{"
                "\"masse_totale_kg\":%.0f,\"masse_kart_kg\":%.0f,\"masse_passagers_kg\":%.0f,"
                "\"voie_m\":%.2f,\"empattement_m\":%.3f,"
                "\"iz_kgm2\":%.1f,\"xcg_m\":%.2f,\"hcg_m\":%.2f,\"ycg_m\":%.3f,"
                "\"ke_vsrad\":%.4f,\"ra_ohm\":%.2f,\"i_max_a\":%.0f,\"reduction\":%.2f,\"rendement\":%.2f,"
                "\"roulement_n\":%.0f,\"amort_lacet\":%.1f,"
                "\"batt_v0\":%.1f,\"batt_rint\":%.2f,\"pente_deg\":%.1f}}\n",
                sc->name.c_str(), sc->desc.c_str(), sc->duration_s,
                vmeta.aTip(), [&]{ KartConfig c; c.setDefaults(); if (sc->cfg) sc->cfg(c); return c.duty_cap_frac; }(),
                p.mass(), p.mass_kart_kg, p.mass_pass_kg,
                p.track_m, p.wb_m, p.iz_kgm2, p.xcg_m, p.hcg_m, p.ycg_m,
                p.ke, p.ra_ohm, p.i_max_a, p.gear, p.eta, p.roll_n, p.yaw_damp,
                p.batt_v0, p.batt_rint, p.slope_rad * 180.f / PI_F);
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    int frame = 0;
    runScenario(*sc, [&](const Vehicle& v, const SimController& c, const CtrlTelemetry& t) {
        if (0 != (frame++ % 8)) return;   // 500 Hz → ~60 Hz display
        printFrame(v, c, t);
        if (realtime)
        {
            const auto target = t0 + std::chrono::microseconds(static_cast<int64_t>(v.t() * 1e6f));
            std::this_thread::sleep_until(target);
        }
    });
    return 0;
}
} // namespace

// ─────────────────────────── MANUAL driving (keyboard via the viewer) ───────────────────────────
// Reads commands (JSON lines {"x":..,"y":..,"start":..,"estop":..,"a":..}) on stdin, drives
// the kart in real time on the HILLY TERRAIN (terrain.hpp): the slope under the kart feeds
// the physics at each step. Ends when stdin closes (the browser is gone).
void parseCmd(const std::string& line, PadCmd& cmd)
{
    auto num = [&](const char* key, float def) {
        const size_t p = line.find(key);
        return (p == std::string::npos) ? def : static_cast<float>(std::atof(line.c_str() + p + std::strlen(key)));
    };
    cmd.x = std::fmax(-1.f, std::fmin(1.f, num("\"x\":", 0.f)));
    cmd.y = std::fmax(-1.f, std::fmin(1.f, num("\"y\":", 0.f)));
    cmd.start = num("\"start\":", 0.f) != 0.f;
    cmd.estop = num("\"estop\":", 0.f) != 0.f;
    cmd.drive = num("\"a\":", 0.f) != 0.f;   // A = hold to drive (the viewer's key)
}

int driveInteractive()
{
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    KartConfig cfg;
    cfg.setDefaults();
    Vehicle veh;
    // 🚪 One-way: drive into the shed and ground_fn stops returning the terrain. The kart is
    // then well above the new floor, the ballistic physics takes over, and it falls 8 m.
    bool backrooms = false;
    veh.ground_fn = [&backrooms](float x, float y) {
        return backrooms ? BACKROOMS_FLOOR : terrainH(x, y);
    };   // jumps possible!
    // Solid walls: the shed's three closed faces up top (enter by the DOOR or not at all),
    // the procedural wall grid down below. Same layout the viewer draws (terrain.hpp).
    veh.wall_fn = [&backrooms](float x, float y) {
        return backrooms ? backroomsWallAt(x, y) : shedWallAt(x, y);
    };
    PadCmd cmd;
    SimController ctrl(veh, [&cmd](float) { return cmd; });

    const VehicleParams& p = veh.params();
    std::printf("{\"meta\":true,\"name\":\"conduite_manuelle\","
                "\"desc\":\"Keyboard driving on hilly terrain (slopes up to ~13 %%)\","
                "\"duration\":0,\"terrain\":true,\"atip\":%.3f,\"cap\":%.2f,\"params\":{"
                "\"masse_totale_kg\":%.0f,\"masse_kart_kg\":%.0f,\"masse_passagers_kg\":%.0f,"
                "\"voie_m\":%.2f,\"empattement_m\":%.3f,\"iz_kgm2\":%.1f,"
                "\"xcg_m\":%.2f,\"hcg_m\":%.2f,\"ycg_m\":%.3f,"
                "\"ke_vsrad\":%.4f,\"ra_ohm\":%.2f,\"i_max_a\":%.0f,\"reduction\":%.2f,"
                "\"rendement\":%.2f,\"roulement_n\":%.0f,\"amort_lacet\":%.1f,"
                "\"batt_v0\":%.1f,\"batt_rint\":%.2f,\"pente_deg\":0}}\n",
                veh.aTip(), cfg.duty_cap_frac,
                p.mass(), p.mass_kart_kg, p.mass_pass_kg,
                p.track_m, p.wb_m, p.iz_kgm2, p.xcg_m, p.hcg_m, p.ycg_m,
                p.ke, p.ra_ohm, p.i_max_a, p.gear, p.eta, p.roll_n, p.yaw_damp,
                p.batt_v0, p.batt_rint);
    std::fflush(stdout);

    std::string acc;
    char buf[512];
    const auto t0 = std::chrono::steady_clock::now();
    for (long frame = 0;; ++frame)
    {
        ssize_t n;
        while ((n = read(0, buf, sizeof(buf))) > 0) acc.append(buf, static_cast<size_t>(n));
        if (0 == n) break;   // EOF: the relay closed (tab gone / scenario changed)
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos)
        {
            const std::string line = acc.substr(0, nl);
            parseCmd(line, cmd);
            // Mixing selector (viewer dropdown): same parameter as the real kart
            // (mix_type) — 0 linear, 1 expo. Applied on the fly, so the feel difference can
            // be compared mid-drive.
            const size_t pm = line.find("\"mx\":");
            if (pm != std::string::npos)
            {
                const int mx = static_cast<int>(std::atof(line.c_str() + pm + 5));
                cfg.mix_type = (mx >= 0 && mx <= 1) ? mx : 0;
            }
            acc.erase(0, nl + 1);
        }

        // 🚪 No exit. Past the doorway the floor is gone and the carpet is damp.
        if (!backrooms && inShed(veh.x(), veh.y()))
        {
            backrooms = true;
            veh.params().roll_n = BACKROOMS_ROLL_N;
        }
        // The REAL slope under the kart drives the physics (uphill brakes, downhill runs away).
        // The Backrooms are endless and flat: no slope down there, ever.
        veh.params().slope_rad = backrooms
            ? 0.f : terrainSlopeAlong(veh.x(), veh.y(), veh.heading());
        ctrl.stepOnce(cfg);

        if (0 == (frame % 8)) printFrame(veh, ctrl, ctrl.telemetry(), backrooms);
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(2000 * (frame + 1)));
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && 0 == std::strcmp(argv[1], "--list"))
    {
        for (const auto& s : allScenarios())
            std::printf("%-24s %s\n", s.name.c_str(), s.desc.c_str());
        return 0;
    }
    if (argc >= 2 && 0 == std::strcmp(argv[1], "--drive"))
    {
        return driveInteractive();
    }
    if (argc >= 3 && 0 == std::strcmp(argv[1], "--stream"))
    {
        const bool realtime = (argc >= 4 && 0 == std::strcmp(argv[3], "--realtime"));
        return streamScenario(argv[2], realtime);
    }

    const char* trace = std::getenv("KART_SIM_TRACE");
    if (trace)
    {
        g_trace = std::fopen(trace, "w");
        if (g_trace)
            std::fprintf(g_trace, "scenario,t,v,w,alat,margin,outl,outr,fwd,turn,state,stop,brake\n");
    }

    std::printf("Physics simulation (real controller + vehicle model):\n");
    testCoreOutputs();
    testScenarios();
    testCapSweep();
    if (g_trace) std::fclose(g_trace);

    if (0 == g_failures)
    {
        std::printf("All simulation scenarios PASS ✔\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
