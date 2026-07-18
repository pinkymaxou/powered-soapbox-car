// scenarios.hpp — Scénarios de simulation PARTAGÉS entre les tests automatisés (sim_main,
// asserts) et le visualisateur 3D temps réel (mode --stream). Chaque scénario = réglages
// (config + véhicule) + manette scriptée + durée. Extrêmes ET réalistes.
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
    std::function<void(KartConfig&)> cfg = nullptr;   // mutations de config (défauts sinon)
    std::function<void(Vehicle&)>    veh = nullptr;   // pente, pannes capteurs, batterie…
    PadScript   pad;
    bool        calibrated = true;
};

// Armement standard : START tenu de 0,2 s à 1,5 s (arm_hold_ms = 1000 par défaut),
// stick au centre pendant l'appui. La conduite scriptée commence donc à t ≥ 1,6 s.
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

// Agrégats d'un run — la matière des asserts.
struct RunResult
{
    float min_tip_margin = 1e9f;   // marge de renversement minimale rencontrée (m/s²)
    float max_v = 0.f;             // |vitesse| max atteinte
    float max_alat = 0.f;
    float final_v = 0.f;
    float stop_dist = -1.f;        // distance parcourue depuis le début du freinage (si mesurée)
    bool  ever_armed = false;
    bool  ever_fault = false;
    Fault final_fault = Fault::None;
    int   batt_type = 0;
    bool  powered_off = false;
    float t_first_fault = -1.f;
    float t_disarmed_after = -1.f;   // date du premier désarmement APRÈS avoir été armé
};

// Exécute un scénario tick par tick ; `hook` (optionnel) est appelé à chaque pas —
// c'est là que se branchent le flux JSON du visualisateur ou une trace CSV.
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
        const CtrlTelemetry& t = ctrl.telemetry();

        r.min_tip_margin = std::min(r.min_tip_margin, veh.tipMargin());
        r.max_v = std::max(r.max_v, std::fabs(veh.v()));
        r.max_alat = std::max(r.max_alat, std::fabs(veh.aLat()));
        r.ever_armed |= t.armed;
        if (Fault::None != t.fault)
        {
            r.ever_fault = true;
            if (r.t_first_fault < 0.f) r.t_first_fault = veh.t();
        }
        if (was_armed && !t.armed && r.t_disarmed_after < 0.f) r.t_disarmed_after = veh.t();
        was_armed = t.armed;

        if (hook) hook(veh, ctrl, t);
    }
    r.final_v = veh.v();
    r.final_fault = ctrl.telemetry().fault;
    r.batt_type = ctrl.telemetry().batt_type;
    r.powered_off = ctrl.powered_off;
    return r;
}

// ─────────────────────────── Les scénarios ───────────────────────────
inline std::vector<Scenario> allScenarios()
{
    std::vector<Scenario> v;

    // EXTRÊME — LE test du projet : plein gaz puis échelon de virage à fond, protection active.
    v.push_back({
        "virage_pleine_vitesse",
        "Plein gaz jusqu'à vmax puis virage à fond (anti-renversement ACTIF)",
        10.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            c.x = (t >= 5.f) ? 1.f : 0.f;   // échelon de virage à pleine vitesse
            return c;
        }});

    // CONTRE-ÉPREUVE : même manœuvre SANS anti-renversement → doit basculer (valide la mesure).
    v.push_back({
        "virage_sans_protection",
        "Même manœuvre avec turn_limit_en=0 : démontre le renversement évité",
        10.f,
        [](KartConfig& c) { c.turn_limit_en = 0.f; },
        nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            c.x = (t >= 5.f) ? 1.f : 0.f;
            return c;
        }});

    // EXTRÊME légal : pivot sur place (virage 100 %, avance nulle) — stable par conception.
    v.push_back({
        "pivot_surplace",
        "Virage à fond sans avance : pivot sur place, vitesse ≈ 0",
        8.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.x = 1.f;
            return c;
        }});

    // RÉALISTE : slalom soutenu.
    v.push_back({
        "slalom",
        "Avance 60 %, virage sinusoïdal ±80 % à 0,5 Hz",
        14.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 0.6f;
            c.x = 0.8f * std::sin(2.f * PI_F * 0.5f * (t - T_DRIVE));
            return c;
        }});

    // RÉALISTE : conduite d'enfant — à-coups pseudo-aléatoires (déterministes, somme de sinus).
    v.push_back({
        "conduite_enfant",
        "Entrées erratiques : à-coups d'avance et coups de virage brusques",
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

    // Décélération commandée (freinage au stick) : le scénario de la fausse « inversion ».
    v.push_back({
        "freinage_stick",
        "Accélère puis stick plein arrière : plugging — AUCUN défaut EncoderDir attendu",
        10.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            if (t < 5.f)      c.y = 1.f;
            else if (t < 7.f) c.y = -1.f;   // plein arrière lancé (bridé à rev_limit)
            else              c.y = 0.f;
            return c;
        }});

    // Frein PID : relâcher à pleine vitesse → arrêt actif.
    v.push_back({
        "frein_pid_arret",
        "Relâche le stick à vmax : frein PID (consigne 0), distance d'arrêt bornée",
        10.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 5.f) ? 1.f : 0.f;
            return c;
        }});

    // Heartbeat : la manette cesse d'émettre à pleine vitesse.
    v.push_back({
        "heartbeat_perte",
        "Rapports manette coupés à pleine vitesse : désarmement ≤ 250 ms + freinage",
        9.f, nullptr, nullptr,
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 1.f;
            if (t >= 5.f) c.reports = false;   // lien « connecté » mais muet
            return c;
        }});

    // Pannes d'encodeur (l'ARRÊT TOTAL doit tomber, la bonne cause affichée).
    v.push_back({
        "encodeur_inverse",
        "AS5600 gauche câblé à l'envers : Fault::EncoderDir en < 1,5 s de conduite",
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
        "AS5600 droit muet (I2C) : Fault::EncoderAbsent, armement refusé",
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
        "Mesure aberrante (> 8 m/s) : Fault::EncoderMad",
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
        "Encodeurs figés (roue coincée/courroie) : Fault::Encoder après ~1 s de poussée",
        7.f, nullptr,
        [](Vehicle& v) { v.enc_mode_l = EncMode::Stuck; v.enc_mode_r = EncMode::Stuck; },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = 0.8f;
            return c;
        }});

    // LVC : batterie fatiguée qui s'affaisse sous charge (détection 12 V puis coupure).
    v.push_back({
        "lvc_batterie_faible",
        "Batterie 12 V fatiguée : l'affaissement sous charge déclenche la LVC (après anti-rebond)",
        12.f, nullptr,
        [](Vehicle& v) {
            v.params().batt_v0 = 11.2f;     // à vide : au-dessus du seuil (10,5 V)
            v.params().batt_rint = 0.12f;   // usée : s'écroule sous 20-30 A
        },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t > 5.f) ? 1.f : 0.f;    // 3,4 s au repos (détection du type), puis charge
            return c;
        }});

    // Détection 24 V au boot (tension stable 3 s).
    v.push_back({
        "detection_24v",
        "Batterie 24 V (2×12 V série) : type détecté = 24 après 3 s de stabilité",
        6.f, nullptr,
        [](Vehicle& v) { v.params().batt_v0 = 25.6f; },
        [](float t) {
            PadCmd c;
            armPhase(t, c);
            return c;
        }});

    // RÉALISTE : descente, stick relâché — le frein actif retient le kart dans la pente.
    v.push_back({
        "descente_frein",
        "Pente −8° : stick relâché, le frein (PID) doit retenir le kart",
        12.f, nullptr,
        [](Vehicle& v) { v.params().slope_rad = -8.f * PI_F / 180.f; },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 4.f) ? 0.4f : 0.f;   // s'engage doucement puis relâche dans la pente
            return c;
        }});

    // Descente 8 % avec FREIN DYNAMIQUE SEUL (brk_pid_enable=0) : le court-circuit ne peut
    // pas arrêter (force ∝ vitesse) mais doit PLAFONNER la descente à une vitesse terminale
    // rampante — la question du scénario : « est-ce qu'il tient quand même ? »
    v.push_back({
        "descente_frein_dynamique",
        "Pente 8 %, frein dynamique SEUL (sans PID) : vitesse terminale bornée attendue",
        18.f,
        [](KartConfig& c) { c.brk_pid_enable = 0.f; },
        [](Vehicle& veh) { veh.params().slope_rad = -std::atan(0.08f); },
        [](float t) {
            PadCmd c;
            if (armPhase(t, c)) return c;
            c.y = (t < 4.f) ? 0.4f : 0.f;   // s'engage dans la pente puis relâche tout
            return c;
        }});

    // CHARGEMENT ASYMÉTRIQUE — un seul enfant assis sur le siège GAUCHE (pas au centre).
    // 33 kg à +0,20 m (demi-banquette) → y_cg = 33×0,20/(32+33) ≈ +0,10 m ; CG un peu plus
    // bas et plus avant (moins de masse sur la banquette arrière).
    v.push_back({
        "enfant_seul_cote",
        "UN enfant (33 kg) assis à gauche : virage à fond des deux côtés à pleine vitesse",
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
            if (t >= 5.f && t < 8.f)  c.x = 1.f;    // virage droite → charge l'arête GAUCHE (côté chargé)
            if (t >= 8.f)             c.x = -1.f;   // virage gauche → arête droite
            return c;
        }});

    // Un ADULTE (70 kg) à gauche + un enfant (33 kg) à droite : plus lourd, CG plus haut,
    // décalé vers l'adulte : y_cg = (70−33)×0,20/135 ≈ +0,055 m.
    v.push_back({
        "adulte_enfant",
        "Adulte 70 kg à gauche + enfant 33 kg à droite : lourd, CG haut, décalé",
        13.f, nullptr,
        [](Vehicle& veh) {
            auto& p = veh.params();
            p.mass_pass_kg = 103.f;
            p.ycg_m = 0.055f;
            p.hcg_m = 0.44f;   // torse d'adulte
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
