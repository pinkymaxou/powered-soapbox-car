// test_main.cpp — Tests HÔTE (g++, sans ESP-IDF) de la logique pure du firmware :
// PID (pid.hpp), buffer circulaire (ringbuffer.hpp) et math de contrôle (control_math.hpp).
// Lancer : ./run_tests.sh (ou voir le job CI « host-tests »).
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
    ctl::mixArcade(0.f, 1.f, 0.6f, l, r);       // pivot sur place, plafonné par le gain
    CHECK(near(l, -0.6f) && near(r, 0.6f));
    ctl::mixArcade(1.f, 1.f, 0.6f, l, r);       // saturation bornée à ±1
    CHECK(near(l, 0.4f) && near(r, 1.f));
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
    CHECK(near(ctl::turnLimit(3.3f, 0.5f, 3.3f, 0.5f), 0.5f));   // à Vmax → 50 %
    CHECK(near(ctl::turnLimit(9.0f, 0.5f, 3.3f, 0.5f), 0.5f));   // au-delà → plafonné à 50 %
    const float mid = ctl::turnLimit(1.9f, 0.5f, 3.3f, 0.5f);    // milieu de rampe → ~75 %
    CHECK(near(mid, 0.75f, 1e-3f));
    CHECK(near(ctl::turnLimit(2.0f, 0.5f, 0.5f, 0.5f), 0.5f));   // span dégénéré → pas de division/0
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
    Ring<4> ring;
    CHECK(0 == ring.count() && 4 == ring.capacity());

    std::string out;
    ring.appendJson(out, "v");
    CHECK(out == "\"v\":[]");

    ring.push(1); ring.push(2); ring.push(3);
    out.clear(); ring.appendJson(out, "v");
    CHECK(out == "\"v\":[1,2,3]");

    ring.push(4); ring.push(5);   // dépassement → écrase le plus ancien
    CHECK(4 == ring.count());
    out.clear(); ring.appendJson(out, "v");
    CHECK(out == "\"v\":[2,3,4,5]");
}

int main()
{
    test_clampf();
    test_deadzone();
    test_slew();
    test_mix_arcade();
    test_square_map();
    test_turn_limit();
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
