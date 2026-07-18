// sim_main.cpp — Simulation du kart : la VRAIE logique de contrôle (controller_core.cpp)
// pilote un modèle physique (sim/vehicle.hpp) à travers des scénarios extrêmes et réalistes.
//
//   ./sim                       → tous les scénarios en accéléré + balayage de paramètres (CI)
//   ./sim --list                → liste des scénarios (nom + description)
//   ./sim --stream NOM          → un scénario, une ligne JSON par frame (60 Hz) sur stdout
//   ./sim --stream NOM --realtime  → idem, cadencé au temps réel (pour le visualisateur 3D)
//   KART_SIM_TRACE=f.csv ./sim  → trace CSV de chaque scénario des tests (inspection)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "sim/scenarios.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("ÉCHEC  %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

using namespace sim;

namespace
{

// Trace CSV optionnelle (KART_SIM_TRACE) : une section par scénario, régénérable.
FILE* g_trace = nullptr;
void traceHook(const char* scen, const Vehicle& v, const SimController& c, const CtrlTelemetry& t)
{
    static int decim = 0;
    if (0 != (decim++ % 8)) return;   // ~60 Hz suffit à l'inspection
    std::fprintf(g_trace, "%s,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%u\n",
                 scen, v.t(), v.v(), v.yawRate(), v.aLat(), v.tipMargin(),
                 c.lastOutL(), c.lastOutR(), t.fwd, t.turn,
                 static_cast<int>(t.state), static_cast<int>(t.fault),
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
        if (!s) { std::printf("scénario introuvable : %s\n", n); std::exit(2); }
        return *s;
    };

    // LE test du projet : l'anti-renversement tient à pleine vitesse, virage à fond.
    {
        const RunResult r = run(get("virage_pleine_vitesse"));
        std::printf("  virage_pleine_vitesse : vmax=%.2f m/s  a_lat max=%.2f  marge min=%.2f m/s²\n",
                    r.max_v, r.max_alat, r.min_tip_margin);
        CHECK(r.ever_armed);
        CHECK(r.max_v > 2.0f);              // on a vraiment roulé vite
        CHECK(r.min_tip_margin > 0.5f);     // marge de sécurité jamais entamée
        CHECK(!r.ever_fault);
    }

    // Contre-épreuve : sans la protection, la MÊME manœuvre bascule → la mesure est sensible.
    {
        const RunResult r = run(get("virage_sans_protection"));
        std::printf("  virage_sans_protection : marge min=%.2f m/s² (négative attendue)\n",
                    r.min_tip_margin);
        CHECK(r.min_tip_margin < 0.f);      // le kart lèverait une roue : protection justifiée
    }

    // Pivot sur place : légal et stable (vitesse quasi nulle → a_lat quasi nulle).
    {
        const RunResult r = run(get("pivot_surplace"));
        CHECK(r.ever_armed);
        CHECK(std::fabs(r.max_v) < 0.6f);
        CHECK(r.min_tip_margin > 2.f);
        CHECK(!r.ever_fault);
    }

    // Réalistes : slalom et conduite erratique — jamais de bascule, jamais de défaut.
    {
        const RunResult r = run(get("slalom"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(r.min_tip_margin > 0.5f);
    }
    {
        const RunResult r = run(get("conduite_enfant"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(r.min_tip_margin > 0.5f);
    }

    // Freinage au stick (plugging) : PAS de fausse « inversion d'encodeur ».
    {
        const RunResult r = run(get("freinage_stick"));
        CHECK(r.ever_armed);
        CHECK(!r.ever_fault);               // notamment pas d'EncoderDir
        CHECK(r.max_v > 2.0f);
    }

    // Frein PID : arrêt actif après relâche, sans repartir en sens inverse.
    {
        const RunResult r = run(get("frein_pid_arret"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(std::fabs(r.final_v) < 0.2f);   // arrêté à la fin du scénario
    }

    // Heartbeat : perte des rapports à pleine vitesse → désarmé vite, kart s'arrête.
    {
        const RunResult r = run(get("heartbeat_perte"));
        CHECK(r.ever_armed);
        CHECK(r.t_disarmed_after >= 5.f && r.t_disarmed_after < 5.35f);   // ≤ 250 ms + marge
        CHECK(std::fabs(r.final_v) < 0.3f);                               // freiné (dynamique)
    }

    // Pannes d'encodeur → le BON défaut, et arrêt.
    {
        const RunResult r = run(get("encodeur_inverse"));
        CHECK(Fault::EncoderDir == r.final_fault);
        CHECK(r.t_first_fault > 0.f && r.t_first_fault < T_DRIVE + 1.5f);
    }
    {
        const RunResult r = run(get("encodeur_absent"));
        CHECK(Fault::EncoderAbsent == r.final_fault);
        CHECK(!r.ever_armed);               // défaut présent dès le boot → armement refusé
    }
    {
        const RunResult r = run(get("encodeur_fou"));
        CHECK(Fault::EncoderMad == r.final_fault);
    }
    {
        const RunResult r = run(get("roue_bloquee"));
        CHECK(Fault::Encoder == r.final_fault);
    }

    // LVC : type détecté 12 V puis coupure sous charge (pas avant l'anti-rebond).
    {
        const RunResult r = run(get("lvc_batterie_faible"));
        CHECK(12 == r.batt_type);
        CHECK(Fault::Lvc == r.final_fault);
        CHECK(r.t_first_fault > 5.4f);      // charge à 5 s + anti-rebond 500 ms
    }

    // Détection 24 V.
    {
        const RunResult r = run(get("detection_24v"));
        CHECK(24 == r.batt_type);
        CHECK(!r.ever_fault);
    }

    // CHARGEMENT ASYMÉTRIQUE — c'est CE résultat qui a fait abaisser le défaut de turn_hi
    // à 0,35 : à l'ancien réglage (0,50) un CG décalé basculait en manœuvre extrême.
    // Le DÉFAUT protège désormais ces cas ; l'ancien réglage reste testé comme preuve.
    {
        const RunResult r = run(get("enfant_seul_cote"));
        std::printf("  enfant_seul_cote (défaut 0,35) : marge min=%.2f m/s²\n", r.min_tip_margin);
        CHECK(r.min_tip_margin > 0.3f);   // le défaut SÛR protège le chargement décalé
        CHECK(!r.ever_fault);
    }
    {
        Scenario sc = get("enfant_seul_cote");   // copie (l'original sert au visualisateur)
        sc.cfg = [](KartConfig& c) { c.turn_hi = 0.5f; };
        const RunResult r = runScenario(sc);
        std::printf("  enfant_seul_cote (ancien 0,50) : marge min=%.2f m/s² — dangereux, d'où le changement\n",
                    r.min_tip_margin);
        CHECK(r.min_tip_margin < 0.f);    // preuve : l'ancien défaut basculait
    }
    {
        const RunResult r = run(get("adulte_enfant"));
        CHECK(r.min_tip_margin > 0.3f);
        CHECK(!r.ever_fault);
    }
    {
        Scenario sc = get("adulte_enfant");
        sc.cfg = [](KartConfig& c) { c.turn_hi = 0.5f; };
        const RunResult r = runScenario(sc);
        CHECK(r.min_tip_margin < 0.f);
    }

    // Descente −8° : le frein retient le kart une fois le stick relâché.
    {
        const RunResult r = run(get("descente_frein"));
        CHECK(r.ever_armed && !r.ever_fault);
        CHECK(std::fabs(r.final_v) < 0.5f);   // pas d'emballement dans la pente
    }
}

// Balayage de paramètres : toute la plage UTILE des réglages web doit rester sans bascule.
// (C'est la demande d'origine : « tester un ensemble de paramètres ».)
void testParamSweep()
{
    int runs = 0;
    float worst = 1e9f;
    float worst_hi = 0, worst_full = 0, worst_lim = 0;
    for (float turn_hi : {0.3f, 0.5f, 0.7f})
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
    std::printf("  balayage : %d combinaisons, pire marge %.2f m/s² (turn_hi=%.1f "
                "turn_full=%.1f vlim=%.1f)\n", runs, worst, worst_hi, worst_full, worst_lim);
}

// ─────────────────────────── Flux JSON (visualisateur) ───────────────────────────
int streamScenario(const std::string& name, bool realtime)
{
    const auto all = allScenarios();
    const Scenario* sc = findScenario(all, name);
    if (!sc)
    {
        std::fprintf(stderr, "scénario inconnu : %s (voir --list)\n", name.c_str());
        return 2;
    }
    // Le véhicule du scénario (mêmes mutations que dans runScenario) → ses paramètres réels
    // partent dans le méta-message : le bouton « Hypothèses » du visualisateur les affiche
    // depuis LA source de vérité, pas depuis une copie.
    Vehicle vmeta;
    if (sc->veh) sc->veh(vmeta);
    const VehicleParams& p = vmeta.params();
    std::printf("{\"meta\":true,\"name\":\"%s\",\"desc\":\"%s\",\"duration\":%.1f,"
                "\"atip\":%.3f,\"vlim\":%.2f,\"params\":{"
                "\"masse_totale_kg\":%.0f,\"masse_kart_kg\":%.0f,\"masse_passagers_kg\":%.0f,"
                "\"voie_m\":%.2f,\"empattement_m\":%.3f,"
                "\"iz_kgm2\":%.1f,\"xcg_m\":%.2f,\"hcg_m\":%.2f,\"ycg_m\":%.3f,"
                "\"ke_vsrad\":%.4f,\"ra_ohm\":%.2f,\"reduction\":%.0f,\"rendement\":%.2f,"
                "\"roulement_n\":%.0f,\"amort_lacet\":%.1f,"
                "\"batt_v0\":%.1f,\"batt_rint\":%.2f,\"pente_deg\":%.1f}}\n",
                sc->name.c_str(), sc->desc.c_str(), sc->duration_s,
                vmeta.aTip(), [&]{ KartConfig c; c.setDefaults(); if (sc->cfg) sc->cfg(c); return c.speed_limit_ms; }(),
                p.mass(), p.mass_kart_kg, p.mass_pass_kg,
                p.track_m, p.wb_m, p.iz_kgm2, p.xcg_m, p.hcg_m, p.ycg_m,
                p.ke, p.ra_ohm, p.gear, p.eta, p.roll_n, p.yaw_damp,
                p.batt_v0, p.batt_rint, p.slope_rad * 180.f / PI_F);
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    int frame = 0;
    runScenario(*sc, [&](const Vehicle& v, const SimController& c, const CtrlTelemetry& t) {
        if (0 != (frame++ % 8)) return;   // 500 Hz → ~60 Hz d'affichage
        // Encodeurs en TOURS CAPTEUR/MIN : mesuré = ce que voit le contrôleur (EMA, quantification,
        // pannes injectées) ; réel = vérité terrain du modèle — leur divergence VISUALISE la panne.
        const float mps2rpm = 60.f * hw::GEAR_RATIO / (PI_F * hw::WHEEL_DIAM_M);
        std::printf("{\"t\":%.3f,\"x\":%.3f,\"y\":%.3f,\"h\":%.4f,\"v\":%.3f,\"w\":%.3f,"
                    "\"alat\":%.3f,\"margin\":%.3f,\"outl\":%.3f,\"outr\":%.3f,"
                    "\"encl\":%.2f,\"encr\":%.2f,\"encl_vrai\":%.2f,\"encr_vrai\":%.2f,"
                    "\"stickx\":%.2f,\"sticky\":%.2f,"
                    "\"padx\":%.2f,\"pady\":%.2f,\"state\":%d,\"fault\":%d,\"faults\":%u,"
                    "\"brake\":%d,\"armed\":%s,\"vbat\":%.2f}\n",
                    v.t(), v.x(), v.y(), v.heading(), v.v(), v.yawRate(),
                    v.aLat(), v.tipMargin(), c.lastOutL(), c.lastOutR(),
                    t.speed_l * mps2rpm, t.speed_r * mps2rpm,
                    v.wheelV(true) * mps2rpm, v.wheelV(false) * mps2rpm,
                    c.padX(), c.padY(),
                    t.turn, t.fwd, static_cast<int>(t.state), static_cast<int>(t.fault),
                    t.faults, static_cast<int>(t.brake_mode), t.armed ? "true" : "false",
                    t.vbat);
        std::fflush(stdout);
        if (realtime)
        {
            const auto target = t0 + std::chrono::microseconds(static_cast<int64_t>(v.t() * 1e6f));
            std::this_thread::sleep_until(target);
        }
    });
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && 0 == std::strcmp(argv[1], "--list"))
    {
        for (const auto& s : allScenarios())
            std::printf("%-24s %s\n", s.name.c_str(), s.desc.c_str());
        return 0;
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

    std::printf("Simulation physique (contrôleur réel + modèle du véhicule) :\n");
    testScenarios();
    testParamSweep();
    if (g_trace) std::fclose(g_trace);

    if (0 == g_failures)
    {
        std::printf("Tous les scénarios de simulation PASSENT ✔\n");
        return 0;
    }
    std::printf("%d échec(s)\n", g_failures);
    return 1;
}
