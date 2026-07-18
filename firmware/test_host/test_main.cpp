// test_main.cpp — Tests HÔTE (g++, sans ESP-IDF) de la logique pure du firmware :
// PID (pid.hpp), buffer circulaire (ringbuffer.hpp) et math de contrôle (control_math.hpp).
// Lancer : ./run_tests.sh (ou voir le job CI « host-tests »).
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
            std::printf("ÉCHEC  %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
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
    CHECK(near(ctl::deadzone(0.f, dz), 0.f));         // centre → 0
    CHECK(near(ctl::deadzone(dz, dz), 0.f));          // bord de zone morte → 0
    CHECK(near(ctl::deadzone(-dz, dz), 0.f));
    CHECK(near(ctl::deadzone(1.f, dz), 1.f));         // pleine course → 1 (remappage continu)
    CHECK(near(ctl::deadzone(-1.f, dz), -1.f));
    CHECK(ctl::deadzone(0.5f, dz) > 0.f);             // monotone, signe conservé
    CHECK(ctl::deadzone(-0.5f, dz) < 0.f);
    CHECK(near(ctl::deadzone(0.5f, dz), -ctl::deadzone(-0.5f, dz)));   // symétrie
}

static void test_slew()
{
    // Pas limité à rate·dt, dans les deux sens ; atteint la cible sans dépassement.
    CHECK(near(ctl::slew(1.f, 0.f, 2.f, 0.1f), 0.2f));
    CHECK(near(ctl::slew(-1.f, 0.f, 2.f, 0.1f), -0.2f));
    CHECK(near(ctl::slew(0.1f, 0.f, 2.f, 0.1f), 0.1f));   // cible plus proche que le pas → atteinte
    float v = 0.f;
    for (int i = 0; i < 100; ++i) v = ctl::slew(0.7f, v, 2.f, 0.01f);
    CHECK(near(v, 0.7f));
}

static void test_mix_arcade()
{
    float l = 0.f, r = 0.f;
    ctl::mixArcade(1.f, 0.f, 0.6f, l, r);       // tout droit → symétrique
    CHECK(near(l, 1.f) && near(r, 1.f));
    ctl::mixArcade(0.f, 1.f, 0.6f, l, r);       // stick à droite → pivot vers la DROITE
    CHECK(near(l, 0.6f) && near(r, -0.6f));     // (gauche accélère, droite recule)
    ctl::mixArcade(0.f, -1.f, 0.6f, l, r);      // stick à gauche → la roue DROITE accélère
    CHECK(near(l, -0.6f) && near(r, 0.6f));
    ctl::mixArcade(1.f, 1.f, 0.6f, l, r);       // saturation bornée à ±1
    CHECK(near(l, 1.f) && near(r, 0.4f));
    ctl::mixArcade(-1.f, 0.f, 0.6f, l, r);      // marche arrière
    CHECK(near(l, -1.f) && near(r, -1.f));
}

static void test_square_map()
{
    float x, y;
    x = 1.f; y = 0.f; ctl::squareMap(x, y);                 // axes : inchangés
    CHECK(near(x, 1.f) && near(y, 0.f));
    x = 0.f; y = -1.f; ctl::squareMap(x, y);
    CHECK(near(x, 0.f) && near(y, -1.f));
    x = 0.f; y = 0.f; ctl::squareMap(x, y);                 // centre : inchangé
    CHECK(near(x, 0.f) && near(y, 0.f));
    const float d = 0.70710678f;                            // diagonale pleine (sur le cercle)
    x = d; y = d; ctl::squareMap(x, y);                     // → coin du carré
    CHECK(near(x, 1.f, 1e-3f) && near(y, 1.f, 1e-3f));
    x = -d; y = d; ctl::squareMap(x, y);
    CHECK(near(x, -1.f, 1e-3f) && near(y, 1.f, 1e-3f));
    x = 0.3f; y = 0.4f; ctl::squareMap(x, y);               // direction conservée (x/y constant)
    CHECK(near(x / y, 0.75f, 1e-3f));
    CHECK(std::fabs(x) <= 1.f && std::fabs(y) <= 1.f);      // toujours borné
}

static void test_turn_limit()
{
    // Rampe anti-renversement : ±100 % sous v_full, décroissance linéaire jusqu'à hi à v_max.
    CHECK(near(ctl::turnLimit(0.0f, 0.5f, 3.3f, 0.5f), 1.f));    // pivot sur place → 100 %
    CHECK(near(ctl::turnLimit(0.5f, 0.5f, 3.3f, 0.5f), 1.f));    // au seuil → encore 100 %
    CHECK(near(ctl::turnLimit(1.0f, 0.5f, 3.3f, 0.5f), 1.f));    // 1/v > 100 % → encore à fond
    CHECK(near(ctl::turnLimit(3.3f, 0.5f, 3.3f, 0.5f), 0.5f));   // à Vmax → 50 %
    CHECK(near(ctl::turnLimit(9.0f, 0.5f, 3.3f, 0.5f), 0.5f * 3.3f / 9.f));   // emballement → se resserre encore (iso-a_lat)
    const float mid = ctl::turnLimit(1.9f, 0.5f, 3.3f, 0.5f);    // mi-vitesse → hi·vmax/v ≈ 87 %
    CHECK(near(mid, 0.5f * 3.3f / 1.9f, 1e-3f));
    CHECK(near(ctl::turnLimit(2.0f, 0.5f, 0.5f, 0.5f), 0.125f)); // v_max ≤ v_full → 1/v direct, pas de division/0
}

static void test_duty_cap_volts()
{
    // Plafond PWM auto = V_nom / Vbat mesurée ; 1 (pas de bridage) si tension inconnue ou basse.
    CHECK(near(ctl::dutyCapVolts(0.f, 12.f), 1.f));      // capteur absent (Vbat inconnue)
    CHECK(near(ctl::dutyCapVolts(-1.f, 12.f), 1.f));
    CHECK(near(ctl::dutyCapVolts(11.5f, 12.f), 1.f));    // batterie moto déchargée → plein duty
    CHECK(near(ctl::dutyCapVolts(12.f, 12.f), 1.f));     // 12 V pile → 100 %
    CHECK(near(ctl::dutyCapVolts(20.f, 12.f), 0.6f));    // pack outil 20 V → 60 %
    CHECK(near(ctl::dutyCapVolts(24.f, 12.f), 0.5f));    // 24 V → 50 %
    CHECK(near(ctl::dutyCapVolts(28.8f, 12.f), 12.f / 28.8f));   // 2×12 V en charge
}

static void test_batt_detect()
{
    const int64_t S = 3000000;   // 3 s de stabilité exigée
    const float TOL = 0.5f, V24 = 18.f;
    const int64_t DT = 2000;     // 500 Hz

    // Batterie 12 V stable → classée 12 après 3 s, pas avant.
    ctl::BattDetect d;
    int64_t t = 0;
    for (int i = 0; i < 1400; ++i) { d.update(12.6f, t, S, TOL, V24); t += DT; }
    CHECK(0 == d.volts);                     // 2,8 s : pas encore
    for (int i = 0; i < 200; ++i) { d.update(12.6f, t, S, TOL, V24); t += DT; }
    CHECK(12 == d.volts);

    // Batterie 24 V (2×12 plomb, repos ~25,3 V) → classée 24.
    ctl::BattDetect d24; t = 0;
    for (int i = 0; i < 1600; ++i) { d24.update(25.3f, t, S, TOL, V24); t += DT; }
    CHECK(24 == d24.volts);

    // Tension INSTABLE (>|tol|) → la fenêtre repart, pas de classification.
    ctl::BattDetect du; t = 0;
    for (int i = 0; i < 3000; ++i) { du.update((i % 2) ? 12.f : 13.f, t, S, TOL, V24); t += DT; }
    CHECK(0 == du.volts);
    // …puis la tension se stabilise → classée.
    for (int i = 0; i < 1600; ++i) { du.update(12.5f, t, S, TOL, V24); t += DT; }
    CHECK(12 == du.volts);

    // Mesure invalide (capteur absent) au milieu → on repart de zéro, sans classer.
    ctl::BattDetect di; t = 0;
    for (int i = 0; i < 1000; ++i) { di.update(12.6f, t, S, TOL, V24); t += DT; }
    di.update(-1.f, t, S, TOL, V24); t += DT;
    for (int i = 0; i < 1400; ++i) { di.update(12.6f, t, S, TOL, V24); t += DT; }
    CHECK(0 == di.volts);                    // 2,8 s depuis la coupure : pas encore

    // Une fois classée, la détection est DÉFINITIVE (on ne change pas de batterie allumé).
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

    // FREINAGE AU STICK : kart à +2 m/s, commande -0,5 → la vitesse opposée FOND vers 0.
    // Décélération 2 m/s² : PAS une inversion, aucun déclenchement attendu.
    ctl::RevDetect d;
    int64_t t = 0; float v = 2.0f; bool fired = false;
    while (v > 0.f)
    {
        fired |= d.update(-0.5f, v, t, WIN, OUT_MIN, V_MIN, DECAY);
        v -= 2.0f * 0.002f;   // 2 m/s² × 2 ms
        t += DT;
    }
    CHECK(!fired);

    // VRAIE INVERSION : commande +0,5, roue mesurée à -1 m/s STABLE → confirmé après 400 ms.
    ctl::RevDetect di; t = 0; fired = false;
    for (int i = 0; i < 300; ++i) { fired |= di.update(0.5f, -1.0f, t, WIN, OUT_MIN, V_MIN, DECAY); t += DT; }
    CHECK(fired);

    // Inversion avec vitesse CROISSANTE (le kart accélère, capteur à l'envers) → confirmé.
    ctl::RevDetect dc; t = 0; v = -0.4f; fired = false;
    for (int i = 0; i < 400; ++i)
    {
        fired |= dc.update(0.6f, v, t, WIN, OUT_MIN, V_MIN, DECAY);
        v -= 1.0f * 0.002f;   // accélère dans le mauvais sens
        t += DT;
    }
    CHECK(fired);

    // Opposition brève (< 400 ms) puis retour normal → rien.
    ctl::RevDetect db; t = 0; fired = false;
    for (int i = 0; i < 100; ++i) { fired |= db.update(0.5f, -1.0f, t, WIN, OUT_MIN, V_MIN, DECAY); t += DT; }
    for (int i = 0; i < 300; ++i) { fired |= db.update(0.5f, 1.0f, t, WIN, OUT_MIN, V_MIN, DECAY); t += DT; }
    CHECK(!fired);
}

// ───────────────────────── Pid ─────────────────────────
static void test_pid()
{
    Pid pid;

    // P pur : sortie proportionnelle à l'erreur, bornée.
    pid.reset();
    CHECK(near(pid.update(1.f, 0.f, 0.01f, 0.5f, 0.f, 0.f, -1.f, 1.f), 0.5f));
    CHECK(near(pid.update(10.f, 0.f, 0.01f, 0.5f, 0.f, 0.f, -1.f, 1.f), 1.f));   // clamp haut

    // Intégrale : accumule vers la consigne (système figé → la sortie monte).
    pid.reset();
    const float o1 = pid.update(1.f, 0.f, 0.1f, 0.f, 1.f, 0.f, -1.f, 1.f);
    const float o2 = pid.update(1.f, 0.f, 0.1f, 0.f, 1.f, 0.f, -1.f, 1.f);
    CHECK(o2 > o1);

    // Anti-windup : après une LONGUE saturation, une inversion d'erreur doit décoller
    // immédiatement du max (l'intégrale a été gelée, pas gonflée).
    pid.reset();
    for (int i = 0; i < 1000; ++i) pid.update(10.f, 0.f, 0.01f, 1.f, 1.f, 0.f, -1.f, 1.f);
    const float after = pid.update(0.f, 10.f, 0.01f, 1.f, 1.f, 0.f, -1.f, 1.f);
    CHECK(after < 1.f);   // un PID « windé » resterait collé à +1

    // reset() efface bien l'état.
    pid.reset();
    CHECK(near(pid.update(0.f, 0.f, 0.01f, 1.f, 1.f, 1.f, -1.f, 1.f), 0.f));
}

// ───────────────────────── Ring ─────────────────────────
static void test_ring()
{
    Ring<uint8_t, 4> ring;
    CHECK(0 == ring.count() && 4 == ring.capacity());

    uint8_t lin[8] = {};
    CHECK(0 == ring.copyTo(lin, 8));                // vide → rien

    ring.push(1); ring.push(2); ring.push(3);
    CHECK(3 == ring.count());
    CHECK(3 == ring.copyTo(lin, 8));
    CHECK(1 == lin[0] && 2 == lin[1] && 3 == lin[2]);

    ring.push(4); ring.push(5);   // dépassement → écrase le plus ancien
    CHECK(4 == ring.count());
    // Linéarisation (pour l'encodage protobuf « bytes ») : plus ancien → plus récent.
    CHECK(4 == ring.copyTo(lin, 8));
    CHECK(2 == lin[0] && 3 == lin[1] && 4 == lin[2] && 5 == lin[3]);
    CHECK(2 == ring.copyTo(lin, 2));                // capacité < count → tronqué au plus ancien
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
        std::printf("Tous les tests hôte PASSENT ✔\n");
        return 0;
    }
    std::printf("%d échec(s)\n", g_failures);
    return 1;
}
