// scenarios.hpp — Simulation scenarios SHARED between the automated tests (sim_main,
// asserts) and the real-time 3D viewer (--stream mode). Each scenario = settings
// (config + vehicle) + scripted gamepad + duration. Extreme AND realistic.
#pragma once

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "sim_controller.hpp"
#include "vehicle.hpp"

namespace sim
{

struct Scenario
{
    std::string name;
    std::string desc;
    float       duration_s = 10.f;
    std::function<void(KartConfig&)> cfg = nullptr;   // config mutations (defaults otherwise)
    std::function<void(Vehicle&)>    veh = nullptr;   // slope, sensor failures, battery…
    PadScript   pad;
    bool        calibrated = true;
};

// Standard arming: START held from 0.2 s to 1.5 s (arm_hold_ms = 1000 by default),
// stick centered during the press. Scripted driving therefore starts at t ≥ 1.6 s.
constexpr float T_DRIVE = 1.6f;
inline bool armPhase(float t, PadCmd& c)
{
    if (t < T_DRIVE)
    {
        c.start = held(t, 0.2f, 1.3f);
        c.x = 0.f;
        c.y = 0.f;
        return true;
    }
    return false;
}

// Aggregates of a run — the material for the asserts.
struct RunResult
{
    float min_tip_margin = 1e9f;   // minimum tip margin encountered (m/s²)
    float max_v = 0.f;             // max |speed| reached
    float max_alat = 0.f;
    float final_v = 0.f;
    float stop_dist = -1.f;        // distance traveled since the start of braking (if measured)
    bool  ever_armed = false;
    bool  ever_fault = false;
    bool  wheel_lifted = false;   // a wheel left the ground (even briefly)
    bool  tipped = false;         // tipped (point of no return crossed)
    Fault final_fault = Fault::None;
    int   batt_type = 0;
    bool  powered_off = false;
    float t_first_fault = -1.f;
    float t_disarmed_after = -1.f;   // time of the first disarm AFTER having been armed
};

// Runs a scenario tick by tick; `hook` (optional) is called at each step —
// this is where the viewer's JSON stream or a CSV trace hook in.
using FrameHook = std::function<void(const Vehicle&, const SimController&, const CtrlTelemetry&)>;

inline RunResult runScenario(const Scenario& sc, const FrameHook& hook = nullptr)
{
    KartConfig cfg;
    cfg.setDefaults();
    if (sc.cfg) sc.cfg(cfg);

    Vehicle veh;
    if (sc.veh) sc.veh(veh);

    SimController ctrl(veh, sc.pad);
    ctrl.calibrated = sc.calibrated;

    RunResult r;
    bool was_armed = false;
    const int steps = static_cast<int>(sc.duration_s * hw::CTRL_HZ);
    for (int i = 0; i < steps; ++i)
    {
        ctrl.stepOnce(cfg);
        const CtrlTelemetry t = ctrl.telemetry();

        r.min_tip_margin = std::min(r.min_tip_margin, veh.tipMargin());
        r.max_v = std::max(r.max_v, std::fabs(veh.v()));
        r.max_alat = std::max(r.max_alat, std::fabs(veh.aLat()));
        r.ever_armed |= t.armed;
        r.wheel_lifted |= (0 != veh.liftSide());
        r.tipped |= veh.tipped();
        if (0 != (t.faults & fb::BLOCKING))
        {
            r.ever_fault = true;
            if (r.t_first_fault < 0.f) r.t_first_fault = veh.t();
        }
        if (was_armed && !t.armed && r.t_disarmed_after < 0.f) r.t_disarmed_after = veh.t();
        was_armed = t.armed;

        if (hook) hook(veh, ctrl, t);
    }
    r.final_v = veh.v();
    r.final_fault = primaryFault(ctrl.telemetry().faults);
    r.batt_type = ctrl.telemetry().batt_type;
    r.powered_off = ctrl.powered_off;
    return r;
}

// ─────────────────────────── The scenarios ───────────────────────────
inline std::vector<Scenario> allScenarios()
{
    std::vector<Scenario> v;

    // EXTREME — THE project test: full throttle then a hard-turn step, protection active.
    v.push_back({
        "virage_pleine_vitesse",
        "Full throttle up to vmax then hard turn (rollover protection ACTIVE)",
        10.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            c.x = (t >= 5.f) ? 1.f : 0.f;   // turn step at full speed
            return c;
        }});

    // COUNTER-TEST: same maneuver WITHOUT rollover protection → must tip (validates the measurement).
    v.push_back({
        "virage_sans_protection",
        "Same maneuver with turn_limit_en=0: demonstrates the avoided rollover",
        10.f,
        [](KartConfig& c) { c.turn_limit_en = 0; },
        nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            c.x = (t >= 5.f) ? 1.f : 0.f;
            return c;
        }});

    // Legal EXTREME: pivot in place (100% turn, zero forward) — stable by design.
    v.push_back({
        "pivot_surplace",
        "Hard turn with no forward motion: pivot in place, speed ≈ 0",
        8.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.x = 1.f;
            return c;
        }});

    // REALISTIC: sustained slalom.
    v.push_back({
        "slalom",
        "60% forward, sinusoidal turn ±80% at 0.5 Hz",
        14.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 0.6f;
            c.x = 0.8f * std::sin(2.f * PI_F * 0.5f * (t - T_DRIVE));
            return c;
        }});

    // REALISTIC: child driving — pseudo-random jerks (deterministic, sum of sines).
    v.push_back({
        "conduite_enfant",
        "Erratic inputs: forward jerks and abrupt turn jabs",
        16.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            const float u = t - T_DRIVE;
            const float a = 0.5f + 0.5f * std::sin(2.3f * u) * std::sin(0.7f * u + 1.f);
            const float s = std::sin(3.1f * u) + 0.8f * std::sin(1.3f * u + 2.f);
            c.y = std::fmax(-0.6f, std::fmin(1.f, a + ((std::fmod(u, 3.7f) < 0.25f) ? -1.5f : 0.f)));
            c.x = std::fmax(-1.f, std::fmin(1.f, s));
            return c;
        }});

    // Commanded deceleration (stick braking): the scenario of the false "reversal".
    v.push_back({
        "freinage_stick",
        "Accelerate then stick fully back: plugging — NO EncoderDir fault expected",
        10.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            if (t < 5.f)      c.y = 1.f;
            else if (t < 7.f) c.y = -1.f;   // full reverse while moving (plugging)
            else              c.y = 0.f;
            return c;
        }});

    // Reverse: full reverse — held by ITS OWN speed limit (rev_speed_ms,
    // same PID, target chosen by the measured direction), independent of the forward limit
    // (left at 3.3). 2 m/s to prove the control loop (the motors saturate around 3).
    v.push_back({
        "marche_arriere",
        "Full reverse: the speed converges on rev_speed_ms (2 m/s), no fault",
        12.f,
        [](KartConfig& c) { c.rev_speed_ms = 2.f; },
        nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = -1.f;
            return c;
        }});

    // Active (PID) braking: release at full speed → active stop.
    v.push_back({
        "frein_pid_arret",
        "Release the stick at vmax: active (PID) braking (command 0), bounded stopping distance",
        10.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 5.f) ? 1.f : 0.f;
            return c;
        }});

    // Heartbeat: the gamepad stops transmitting at full speed.
    v.push_back({
        "heartbeat_perte",
        "Gamepad reports cut at full speed: disarm ≤ 250 ms + braking",
        9.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            if (t >= 5.f) c.reports = false;   // link "connected" but silent
            return c;
        }});

    // Encoder failures (the FULL STOP must trigger, the right cause displayed).
    v.push_back({
        "encodeur_inverse",
        "Left AS5600 wired backwards: Fault::EncoderDir in < 1.5 s of driving",
        8.f, nullptr,
        [](Vehicle& v) { v.enc_mode_l = EncMode::Reversed; },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            return c;
        }});
    v.push_back({
        "encodeur_absent",
        "Right AS5600 silent (I2C): Fault::EncoderAbsent, arming refused",
        5.f, nullptr,
        [](Vehicle& v) { v.enc_mode_r = EncMode::Absent; },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 0.5f;
            return c;
        }});
    v.push_back({
        "encodeur_fou",
        "Aberrant measurement (> 8 m/s): Fault::EncoderMad",
        6.f, nullptr,
        [](Vehicle& v) { v.enc_mode_l = EncMode::Crazy; },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 0.4f;
            return c;
        }});
    v.push_back({
        "roue_bloquee",
        "Stuck encoders (jammed wheel/thrown chain): Fault::Encoder after ~1 s of pushing",
        7.f, nullptr,
        [](Vehicle& v) { v.enc_mode_l = EncMode::Stuck; v.enc_mode_r = EncMode::Stuck; },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 0.8f;
            return c;
        }});

    // LVC: worn battery that sags under load (12 V detection then cutoff).
    v.push_back({
        "lvc_batterie_faible",
        "Worn 12 V battery: the sag under load triggers the LVC (after debounce)",
        12.f, nullptr,
        [](Vehicle& v) {
            v.params().batt_v0 = 11.2f;     // open-circuit: above the threshold (10.5 V)
            v.params().batt_rint = 0.12f;   // worn: collapses under 20-30 A
        },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t > 5.f) ? 1.f : 0.f;    // 3.4 s at rest (type detection), then load
            return c;
        }});

    // 24 V detection at boot (stable voltage 3 s).
    v.push_back({
        "detection_24v",
        "24 V battery (2×12 V series): type detected = 24 after 3 s of stability",
        6.f, nullptr,
        [](Vehicle& v) { v.params().batt_v0 = 25.6f; },
        [](float t) {
            PadCmd c;
            armPhase(t, c);
            return c;
        }});

    // REALISTIC: 8% downhill, stick released — active braking holds the kart on the slope
    // (within the motor capacity: ~134 N of holding force against 77 N of gravity).
    v.push_back({
        "descente_frein",
        "Slope 8%: stick released, the (PID) brake must hold the kart",
        12.f, nullptr,
        [](Vehicle& v) { v.params().slope_rad = -std::atan(0.08f); },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 4.f) ? 0.4f : 0.f;   // eases in then releases on the slope
            return c;
        }});

    // 8% downhill with DYNAMIC BRAKING ONLY (brk_pid_enable=0): the short-circuit cannot
    // stop (force ∝ speed) but must CAP the descent at a crawling terminal
    // speed — the scenario's question: "does it hold anyway?"
    v.push_back({
        "descente_frein_dynamique",
        "Slope 8%, dynamic braking ONLY (without PID): bounded terminal speed expected",
        18.f,
        [](KartConfig& c) { c.brk_pid_enable = 0; },
        [](Vehicle& veh) { veh.params().slope_rad = -std::atan(0.08f); },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 4.f) ? 0.4f : 0.f;   // engages on the slope then releases everything
            return c;
        }});

    // Slope 16% (steep!) — active (PID) braking: must hold the kart near a stop.
    v.push_back({
        "descente16_frein_actif",
        "Slope 16%, active (PID) braking: the kart must be held near a stop",
        18.f, nullptr,
        [](Vehicle& veh) { veh.params().slope_rad = -std::atan(0.16f); },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 4.f) ? 0.4f : 0.f;
            return c;
        }});

    // Slope 16% — dynamic braking only: higher terminal speed (∝ slope), bounded?
    v.push_back({
        "descente16_frein_dynamique",
        "Slope 16%, dynamic braking ONLY: terminal speed ~1 m/s expected",
        18.f,
        [](KartConfig& c) { c.brk_pid_enable = 0; },
        [](Vehicle& veh) { veh.params().slope_rad = -std::atan(0.16f); },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 4.f) ? 0.4f : 0.f;
            return c;
        }});

    // KILL SWITCH ON A SLOPE: power cut off → MOSFETs open → COASTING. Only
    // rolling resistance (~30 N) remains against gravity: on 8% (77 N) as on 16% (152 N),
    // the kart RUNS AWAY — this is the quantified demonstration of the README warning
    // (hardware fix: normally-closed relay across the motors).
    v.push_back({
        "coupure_pente8",
        "Slope 8%: kill switch pressed at t=6 s → coasting, runaway expected",
        14.f, nullptr,
        [](Vehicle& veh) { veh.params().slope_rad = -std::atan(0.08f); },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 5.f) ? 0.3f : 0.f;
            if (t >= 6.f) c.sys_power = false;   // kill switch
            return c;
        }});
    v.push_back({
        "coupure_pente16",
        "Slope 16%: kill switch pressed at t=6 s → fast runaway",
        14.f, nullptr,
        [](Vehicle& veh) { veh.params().slope_rad = -std::atan(0.16f); },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 5.f) ? 0.3f : 0.f;
            if (t >= 6.f) c.sys_power = false;
            return c;
        }});

    // ASYMMETRIC LOAD — a single child sitting on the LEFT seat (not centered).
    // 33 kg at +0.20 m (half-bench) → y_cg = 33×0.20/(32+33) ≈ +0.10 m; CG a bit
    // lower and more forward (less mass on the rear bench).
    v.push_back({
        "enfant_seul_cote",
        "ONE child (33 kg) sitting on the left: hard turn on both sides at full speed",
        13.f, nullptr,
        [](Vehicle& veh) {
            auto& p = veh.params();
            p.mass_pass_kg = 33.f;
            p.ycg_m = 0.10f;
            p.xcg_m = 0.36f;
            p.hcg_m = 0.36f;
        },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            if (t >= 5.f && t < 8.f)  c.x = 1.f;    // turn right → loads the LEFT edge (loaded side)
            if (t >= 8.f)             c.x = -1.f;   // turn left → right edge
            return c;
        }});

    // An ADULT (70 kg) on the left + a child (33 kg) on the right: heavier, higher CG,
    // offset toward the adult: y_cg = (70−33)×0.20/135 ≈ +0.055 m.
    v.push_back({
        "adulte_enfant",
        "Adult 70 kg on the left + child 33 kg on the right: heavy, high CG, offset",
        13.f, nullptr,
        [](Vehicle& veh) {
            auto& p = veh.params();
            p.mass_pass_kg = 103.f;
            p.ycg_m = 0.055f;
            p.hcg_m = 0.44f;   // adult torso
            p.iz_kgm2 = 16.f;
        },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            if (t >= 5.f && t < 8.f)  c.x = 1.f;
            if (t >= 8.f)             c.x = -1.f;
            return c;
        }});

    return v;
}

inline const Scenario* findScenario(const std::vector<Scenario>& all, const std::string& name)
{
    for (const auto& s : all)
        if (s.name == name) return &s;
    return nullptr;
}

} // namespace sim
