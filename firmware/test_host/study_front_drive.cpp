// study_front_drive.cpp — ONE-OFF STUDY (not part of the test suite, not built by CI).
// Question it answers: if the driven pair went back to the FRONT and the free caster to the
// REAR, how far forward would the bench have to move to keep the kart stable with two
// passengers? See doc/motrices-avant.md for the write-up.
//
//   cd firmware/test_host
//   g++ -std=c++17 -O2 -I ../main -I sim study_front_drive.cpp ../main/controller_core.cpp
//       ../main/config_params.cpp -o /tmp/study     (one line) then: /tmp/study
//
// The rollover criterion only knows xcg — the CG's distance to the PAIRED axle — so a layout
// flip enters the model as the xcg it produces, and every other number (track, wheelbase, CG
// height, masses) is the kart as built. Each case therefore runs the REAL controller over the
// REAL manoeuvres of scenarios.hpp, only with the geometry the flip would give.
#include "scenarios.hpp"

#include <cstdio>
#include <string>

using namespace sim;

namespace
{
// Loaded mass 98 kg = 32 kart + 66 passengers; the 8 kg of motors+gearboxes travel with the
// driven wheels. Flipping them to the front pulls the CG one wheelbase × 8/98 = 95 mm forward,
// leaving it 615 mm from the now-front driven axle. Each mm of bench travel then moves the
// loaded CG 66/98 = 0.673 mm.
constexpr float XCG_FLIP_MM = 615.f;
constexpr float BENCH_GAIN  = 0.673f;

struct Case
{
    const char* label;
    float bench_mm;    // bench moved FORWARD from where it sits today
    float hcg_m;       // loaded CG height (0.482 as built)
    float extra_mm;    // CG shift from moving something else (+ = toward the single wheel)
};
} // namespace

int main()
{
    const auto all = allScenarios();
    auto get = [&](const std::string& n) {
        for (const auto& s : all) if (s.name == n) return s;
        std::printf("scenario %s not found\n", n.c_str());
        std::exit(1);
    };

    const Case cases[] = {
        {"flip, bench untouched",      0.f, 0.482f,  0.f},
        {"bench +150 mm (6\")",      150.f, 0.482f,  0.f},
        {"bench +237 mm (9.3\")",    237.f, 0.482f,  0.f},
        {"bench +300 mm (12\")",     300.f, 0.482f,  0.f},
        {"bench +386 mm (15\")",     386.f, 0.482f,  0.f},
        {"+237 mm, seat 50 mm lower",237.f, 0.432f,  0.f},
        {"+237 mm, battery moved aft",237.f,0.482f, 59.f},
    };
    const char* scens[] = {"virage_pleine_vitesse", "virage_sans_protection",
                           "enfant_seul_cote", "adulte_enfant"};

    std::printf("%-28s %7s %6s %6s", "case", "xcg mm", "a_tip", "g");
    for (const char* s : scens) std::printf(" %24s", s);
    std::printf("\n");

    for (const Case& c : cases)
    {
        const float xcg = (XCG_FLIP_MM - BENCH_GAIN * c.bench_mm + c.extra_mm) / 1000.f;
        Vehicle probe;
        probe.params().xcg_m = xcg;
        probe.params().hcg_m = c.hcg_m;
        std::printf("%-28s %7.0f %6.2f %6.2f", c.label, xcg * 1000.f, probe.aTip(), probe.aTip() / 9.81f);

        for (const char* name : scens)
        {
            Scenario sc = get(name);
            const auto base = sc.veh;
            const float h = c.hcg_m;
            sc.veh = [base, xcg, h](Vehicle& v) {
                if (base) base(v);               // the scenario's own load case first…
                v.params().xcg_m = xcg;          // …then the geometry this study is testing
                v.params().hcg_m *= h / 0.482f;  // keep the scenario's own height offset
            };
            const RunResult r = runScenario(sc);
            std::printf(" %11.2f m/s2 %s", r.min_tip_margin,
                        r.tipped ? "TIPPED " : (r.wheel_lifted ? "lifted " : "planted"));
        }
        std::printf("\n");
    }
    std::printf("\nmin_tip_margin = worst (a_tip - |a_lat|) over the run; < 0 = a wheel came up.\n"
                "virage_sans_protection is the COUNTER-TEST (turn limiter off) — the kart as\n"
                "built fails it too (-0.60 m/s2): it is there to show what the limiter carries.\n");
    return 0;
}
