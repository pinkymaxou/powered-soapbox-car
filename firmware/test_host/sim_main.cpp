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
    std::fprintf(g_trace, "%s,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%u\n",
                 scen, v.t(), v.v(), v.yawRate(), v.aLat(), v.tipMargin(),
                 c.lastOutL(), c.lastOutR(), t.fwd, t.turn,
                 static_cast<int>(t.state), static_cast<int>(primaryFault(t.faults)),
                 static_cast<int>(t.brake_mode), t.faults);
}

RunResult run(const Scenario& sc)
{
    if (!g_trace) return runScenario(sc);
    return runScenario(sc, [&](const Vehicle& v, const SimController& c, const CtrlTelemetry& t) {
        traceHook(sc.name.c_str(), v, c, t);
    });
}

// ─────────────────────────── Tests (asserts) ───────────────────────────
void testScenarios()
{
    const auto all = allScenarios();
    auto get = [&](const char* n) -> const Scenario& {
        const Scenario* s = findScenario(all, n);
        if (!s) { std::printf("scenario not found: %s\n", n); std::exit(2); }
        return *s;
    };

    // THE project test: rollover protection holds at full speed, hard turn.
    {
        const RunResult r = run(get("virage_pleine_vitesse"));
        std::printf("  virage_pleine_vitesse : vmax=%.2f m/s  a_lat max=%.2f  min margin=%.2f m/s²\n",
                    r.max_v, r.max_alat, r.min_tip_margin);
        CHECK(r.ever_armed);
        CHECK(r.max_v > 2.0f);              // we really did go fast
        CHECK(r.min_tip_margin > 0.5f);     // safety margin never eaten into
        CHECK(!r.ever_fault);
        CHECK(!r.wheel_lifted && !r.tipped);   // all 3 wheels stay on the ground
    }

    // Counter-test: without the protection, the SAME maneuver tips → the measurement is sensitive.
    {
        const RunResult r = run(get("virage_sans_protection"));
        std::printf("  virage_sans_protection : min margin=%.2f m/s² — %s\n",
                    r.min_tip_margin,
                    r.tipped ? "TIPPED (physics)" : (r.wheel_lifted ? "wheel lifted" : "?"));
        CHECK(r.min_tip_margin < 0.f);
        CHECK(r.wheel_lifted);              // the inner wheel physically leaves the ground
        CHECK(r.tipped);                    // …and the CG crosses the edge: real rollover
    }

    // Pivot in place: legal and stable (near-zero speed → near-zero a_lat).
    {
        const RunResult r = run(get("pivot_surplace"));
        CHECK(r.ever_armed);
        CHECK(std::fabs(r.max_v) < 0.6f);
        CHECK(r.min_tip_margin > 2.f);
        CHECK(!r.ever_fault);
    }

    // Realistic: slalom and erratic driving — never a tip, never a fault.
    {
        const RunResult r = run(get("slalom"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(r.min_tip_margin > 0.5f);
        CHECK(!r.wheel_lifted);
    }
    {
        const RunResult r = run(get("conduite_enfant"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(r.min_tip_margin > 0.5f);
    }

    // Stick braking (plugging): NO false "encoder reversal".
    {
        const RunResult r = run(get("freinage_stick"));
        CHECK(r.ever_armed);
        CHECK(!r.ever_fault);               // in particular no EncoderDir
        CHECK(r.max_v > 2.0f);
    }

    // Reverse: no more PWM cap — the TOTAL speed limit holds in both directions.
    {
        const RunResult r = run(get("marche_arriere"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(std::fabs(r.final_v) > 1.7f);          // genuinely rolling in reverse (not re-capped)
        CHECK(std::fabs(r.final_v) < 2.f * 1.05f);   // CONVERGED on the total limit (±5%)
        CHECK(r.max_v < 2.f * 1.25f);                // bounded PID transient overshoot
        std::printf("  marche_arriere : final v=%.2f m/s, peak %.2f (limit 2.0)\n",
                    r.final_v, r.max_v);
    }

    // Active (PID) braking: active stop after release, without setting off in the opposite direction.
    {
        const RunResult r = run(get("frein_pid_arret"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(std::fabs(r.final_v) < 0.2f);   // stopped by the end of the scenario
    }

    // Heartbeat: loss of reports at full speed → disarmed quickly, kart stops.
    {
        const RunResult r = run(get("heartbeat_perte"));
        CHECK(r.ever_armed);
        CHECK(r.t_disarmed_after >= 5.f && r.t_disarmed_after < 5.35f);   // ≤ 250 ms + margin
        CHECK(std::fabs(r.final_v) < 0.3f);                               // braked (dynamic)
    }

    // Encoder failures → the RIGHT fault, and a stop.
    {
        const RunResult r = run(get("encodeur_inverse"));
        CHECK(Fault::EncoderDir == r.final_fault);
        CHECK(r.t_first_fault > 0.f && r.t_first_fault < T_DRIVE + 1.5f);
    }
    {
        const RunResult r = run(get("encodeur_absent"));
        CHECK(Fault::EncoderAbsent == r.final_fault);
        CHECK(!r.ever_armed);               // fault present from boot → arming refused
    }
    {
        const RunResult r = run(get("encodeur_fou"));
        CHECK(Fault::EncoderMad == r.final_fault);
    }
    {
        const RunResult r = run(get("roue_bloquee"));
        CHECK(Fault::Encoder == r.final_fault);
    }

    // LVC: type detected 12 V then cutoff under load (not before debounce).
    {
        const RunResult r = run(get("lvc_batterie_faible"));
        CHECK(12 == r.batt_type);
        CHECK(Fault::Lvc == r.final_fault);
        CHECK(r.t_first_fault > 5.4f);      // load at 5 s + 500 ms debounce
    }

    // 24 V detection.
    {
        const RunResult r = run(get("detection_24v"));
        CHECK(24 == r.batt_type);
        CHECK(!r.ever_fault);
    }

    // ASYMMETRIC LOAD — it is THIS result that lowered the turn_hi default
    // (defaults: gain 1.0 + iso-a_lat 0.2); with the old linear ramp an offset CG would tip.
    // The DEFAULT now protects these cases; the old setting stays tested as proof.
    {
        const RunResult r = run(get("enfant_seul_cote"));
        std::printf("  enfant_seul_cote (defaults) : min margin=%.2f m/s²\n", r.min_tip_margin);
        CHECK(r.min_tip_margin > 0.3f);   // the SAFE default protects the offset load
        CHECK(!r.ever_fault);
        CHECK(!r.wheel_lifted && !r.tipped);
    }
    {
        Scenario sc = get("enfant_seul_cote");   // copy (the original is used by the viewer)
        sc.cfg = [](KartConfig& c) { c.turn_hi = 0.5f; };
        const RunResult r = runScenario(sc);
        std::printf("  enfant_seul_cote (old 0.50) : min margin=%.2f m/s² — %s\n",
                    r.min_tip_margin, r.wheel_lifted ? "WHEEL LIFTED" : "?");
        CHECK(r.min_tip_margin < 0.f);    // proof: the old default would tip
        CHECK(r.wheel_lifted);            // …physically: the inner wheel lifts off
    }
    {
        const RunResult r = run(get("adulte_enfant"));
        std::printf("  adulte_enfant (defaults) : min margin=%.2f m/s²\n", r.min_tip_margin);
        CHECK(r.min_tip_margin > 0.25f);
        CHECK(!r.ever_fault);
    }
    {
        Scenario sc = get("adulte_enfant");
        sc.cfg = [](KartConfig& c) { c.turn_hi = 0.5f; };
        const RunResult r = runScenario(sc);
        CHECK(r.min_tip_margin < 0.f);
    }

    // Downhill 8%: active braking holds the kart (77 N of gravity < ~134 N of capacity).
    {
        const RunResult r = run(get("descente_frein"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(std::fabs(r.final_v) < 0.5f);   // held — within the motor capacity
    }

    // KILL SWITCH ON A SLOPE (power cutoff → coasting): the quantified demonstration of
    // the README warning — with no power there is NO electric braking left AT ALL, and
    // the kart RUNS AWAY down the slope. Documented hardware fix: NC relay across the motors.
    {
        const RunResult r = run(get("coupure_pente8"));
        std::printf("  coupure_pente8 : v(+8 s after kill switch)=%.1f m/s — RUNAWAY\n",
                    std::fabs(r.final_v));
        CHECK(std::fabs(r.final_v) > 3.f);    // coasting: ~4 m/s after 8 s on 8%
    }
    {
        const RunResult r = run(get("coupure_pente16"));
        std::printf("  coupure_pente16 : v(+8 s after kill switch)=%.1f m/s — RUNAWAY\n",
                    std::fabs(r.final_v));
        CHECK(std::fabs(r.final_v) > 8.f);    // ~10 m/s (36 km/h) after 8 s on 16%
    }

    // Slope 16%: BEYOND THE MOTORS' CAPACITY (19.6 A → ~52 N/wheel, i.e. ~134 N of
    // total holding force with rolling resistance, against 152 N of gravity). Active (PID) braking as
    // dynamic braking converge toward the same slip: above ~0.9 m/s both modes
    // are clamped at max current — the kart descends, slowly but inexorably.
    // Lesson: max holdable slope ≈ 11% at full load; 16% demands a MECHANICAL
    // brake (or avoiding the slope).
    {
        const RunResult r = run(get("descente16_frein_actif"));
        std::printf("  descente16 active (PID) brake : final v=%.2f m/s — motor capacity EXCEEDED\n",
                    std::fabs(r.final_v));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(std::fabs(r.final_v) > 3.f);    // does NOT hold: documents the limit
    }
    {
        const RunResult r = run(get("descente16_frein_dynamique"));
        std::printf("  descente16 dynamic brake : final v=%.2f m/s — same (clamped)\n",
                    std::fabs(r.final_v));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(std::fabs(r.final_v) > 3.f);
    }

    // Slope 8%, dynamic braking only: cannot stop (force ∝ v) but must
    // CAP the descent at a crawling terminal speed — no runaway.
    {
        const RunResult r = run(get("descente_frein_dynamique"));
        std::printf("  descente_frein_dynamique (8 %%, short-circuit only) : final v=%.2f m/s\n",
                    std::fabs(r.final_v));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(std::fabs(r.final_v) < 0.7f);   // crawling terminal speed ("it holds")
        CHECK(std::fabs(r.final_v) > 0.05f);  // …but does NOT STOP: documented limit
    }
}

// Parameter sweep: the entire USEFUL range of web settings must remain tip-free.
// (This is the original request: "test a set of parameters".)
void testParamSweep()
{
    int runs = 0;
    float worst = 1e9f;
    float worst_hi = 0, worst_full = 0, worst_lim = 0;
    for (float turn_hi : {0.2f, 0.3f, 0.4f})
        for (float turn_full : {0.3f, 0.5f, 0.8f})
            for (float vlim : {2.0f, 3.3f})
            {
                Scenario sc = *findScenario(allScenarios(), "virage_pleine_vitesse");
                sc.cfg = [=](KartConfig& c) {
                    c.turn_hi = turn_hi;
                    c.turn_full_ms = turn_full;
                    c.speed_limit_ms = vlim;
                };
                const RunResult r = runScenario(sc);
                ++runs;
                if (r.min_tip_margin < worst)
                {
                    worst = r.min_tip_margin;
                    worst_hi = turn_hi; worst_full = turn_full; worst_lim = vlim;
                }
                CHECK(r.min_tip_margin > 0.f);
                CHECK(!r.ever_fault);
            }
    std::printf("  sweep : %d combinations, worst margin %.2f m/s² (turn_hi=%.1f "
                "turn_full=%.1f vlim=%.1f)\n", runs, worst, worst_hi, worst_full, worst_lim);
}

// ─────────────────────────── JSON stream (viewer) ───────────────────────────
// One frame of the stream (shared between scripted scenarios and manual driving).
void printFrame(const Vehicle& v, const SimController& c, const CtrlTelemetry& t)
{
    const float mps2rpm = 60.f * hw::GEAR_RATIO / (PI_F * hw::WHEEL_DIAM_M);
    std::printf("{\"t\":%.3f,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"h\":%.4f,\"v\":%.3f,\"w\":%.3f,"
                "\"alat\":%.3f,\"margin\":%.3f,\"roll\":%.4f,\"pitch\":%.4f,\"lift\":%d,\"tipped\":%s,\"air\":%s,"
                "\"outl\":%.3f,\"outr\":%.3f,"
                "\"encl\":%.2f,\"encr\":%.2f,\"encl_vrai\":%.2f,\"encr_vrai\":%.2f,"
                "\"p_w\":%.1f,\"e_wh\":%.3f,"
                "\"stickx\":%.2f,\"sticky\":%.2f,"
                "\"padx\":%.2f,\"pady\":%.2f,\"state\":%d,\"fault\":%d,\"faults\":%u,"
                "\"brake\":%d,\"armed\":%s,\"power\":%s,\"vbat\":%.2f}\n",
                v.t(), v.x(), v.y(), v.z(), v.heading(), v.v(), v.yawRate(),
                v.aLat(), v.tipMargin(), v.roll(), v.pitch(), v.liftSide(),
                v.tipped() ? "true" : "false", v.airborne() ? "true" : "false",
                c.lastOutL(), c.lastOutR(),
                t.speed_l * mps2rpm, t.speed_r * mps2rpm,
                v.wheelV(true) * mps2rpm, v.wheelV(false) * mps2rpm,
                v.powerW(), v.energyWh(),
                c.padX(), c.padY(),
                t.turn, t.fwd, static_cast<int>(t.state), static_cast<int>(primaryFault(t.faults)),
                t.faults, static_cast<int>(t.brake_mode), t.armed ? "true" : "false",
                c.powered() ? "true" : "false", t.vbat);
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
                "\"atip\":%.3f,\"vlim\":%.2f,\"params\":{"
                "\"masse_totale_kg\":%.0f,\"masse_kart_kg\":%.0f,\"masse_passagers_kg\":%.0f,"
                "\"voie_m\":%.2f,\"empattement_m\":%.3f,"
                "\"iz_kgm2\":%.1f,\"xcg_m\":%.2f,\"hcg_m\":%.2f,\"ycg_m\":%.3f,"
                "\"ke_vsrad\":%.4f,\"ra_ohm\":%.2f,\"i_max_a\":%.0f,\"reduction\":%.0f,\"rendement\":%.2f,"
                "\"roulement_n\":%.0f,\"amort_lacet\":%.1f,"
                "\"batt_v0\":%.1f,\"batt_rint\":%.2f,\"pente_deg\":%.1f}}\n",
                sc->name.c_str(), sc->desc.c_str(), sc->duration_s,
                vmeta.aTip(), [&]{ KartConfig c; c.setDefaults(); if (sc->cfg) sc->cfg(c); return c.speed_limit_ms; }(),
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
// Reads commands (JSON lines {"x":..,"y":..,"start":..,"estop":..}) on stdin, drives
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
}

int driveInteractive()
{
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    KartConfig cfg;
    cfg.setDefaults();
    Vehicle veh;
    veh.ground_fn = [](float x, float y) { return terrainH(x, y); };   // jumps possible!
    PadCmd cmd;
    SimController ctrl(veh, [&cmd](float) { return cmd; });

    const VehicleParams& p = veh.params();
    std::printf("{\"meta\":true,\"name\":\"conduite_manuelle\","
                "\"desc\":\"Keyboard driving on hilly terrain (slopes up to ~13 %%)\","
                "\"duration\":0,\"terrain\":true,\"atip\":%.3f,\"vlim\":%.2f,\"params\":{"
                "\"masse_totale_kg\":%.0f,\"masse_kart_kg\":%.0f,\"masse_passagers_kg\":%.0f,"
                "\"voie_m\":%.2f,\"empattement_m\":%.3f,\"iz_kgm2\":%.1f,"
                "\"xcg_m\":%.2f,\"hcg_m\":%.2f,\"ycg_m\":%.3f,"
                "\"ke_vsrad\":%.4f,\"ra_ohm\":%.2f,\"i_max_a\":%.0f,\"reduction\":%.0f,"
                "\"rendement\":%.2f,\"roulement_n\":%.0f,\"amort_lacet\":%.1f,"
                "\"batt_v0\":%.1f,\"batt_rint\":%.2f,\"pente_deg\":0}}\n",
                veh.aTip(), cfg.speed_limit_ms,
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
            // Rollover protection switch (viewer checkbox): same
            // parameter as on the real kart (turn_limit_en), applied on the fly.
            const size_t p = line.find("\"tl\":");
            if (p != std::string::npos)
            {
                cfg.turn_limit_en = (std::atof(line.c_str() + p + 5) != 0.0) ? 1.f : 0.f;
            }
            acc.erase(0, nl + 1);
        }

        // The REAL slope under the kart drives the physics (uphill brakes, downhill runs away).
        veh.params().slope_rad = terrainSlopeAlong(veh.x(), veh.y(), veh.heading());
        ctrl.stepOnce(cfg);

        if (0 == (frame % 8)) printFrame(veh, ctrl, ctrl.telemetry());
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
            std::fprintf(g_trace, "scenario,t,v,w,alat,margin,outl,outr,fwd,turn,state,fault,brake,faults\n");
    }

    std::printf("Physics simulation (real controller + vehicle model):\n");
    testScenarios();
    testParamSweep();
    if (g_trace) std::fclose(g_trace);

    if (0 == g_failures)
    {
        std::printf("All simulation scenarios PASS ✔\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
