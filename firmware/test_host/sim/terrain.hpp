// terrain.hpp — Terrain vallonné du mode CONDUITE MANUELLE : somme de collines gaussiennes.
// La pente sous le kart (le long du cap) alimente la physique à chaque pas — grimper ralentit,
// descendre emballe, exactement comme les scénarios de pente, mais en continu.
// ⚠️ MIROIR JS : les mêmes collines sont codées dans tools/sim_viewer.html (maillage 3D) —
// garder les deux tables synchronisées.
#pragma once

#include <cmath>

namespace sim
{

struct Hill
{
    float cx, cy;    // centre (m)
    float sigma;     // étalement (m)
    float amp;       // hauteur (m)
};

// Zone de départ (±20 m autour de l'origine) quasi plate ; pentes max ~13 % sur les flancs —
// certaines dépassent donc la capacité de retenue des moteurs (~11 %) : réaliste et instructif.
constexpr Hill TERRAIN_HILLS[] = {
    {  60.f,  20.f, 18.f, 4.0f },
    { -50.f,  40.f, 22.f, 5.0f },
    {  30.f, -70.f, 25.f, 6.0f },
    { -70.f, -60.f, 20.f, 3.5f },
    {  90.f, -30.f, 16.f, 3.0f },
};

// TREMPLIN : rampe de 0,6 m sur 3 m (pente ~20 %) avec ARÊTE FRANCHE à x=17 — franchie à
// pleine vitesse, le kart DÉCOLLE (la physique verticale est balistique une fois en l'air).
inline float rampH(float x, float y)
{
    if (y < -2.5f || y > 2.5f) return 0.f;
    if (x < 14.f || x > 17.f) return 0.f;
    return 0.6f * (x - 14.f) / 3.f;
}

inline float terrainH(float x, float y)
{
    float h = rampH(x, y);
    for (const Hill& c : TERRAIN_HILLS)
    {
        const float dx = x - c.cx, dy = y - c.cy;
        h += c.amp * std::exp(-(dx * dx + dy * dy) / (2.f * c.sigma * c.sigma));
    }
    return h;
}

// Pente du terrain sous le kart, le long du cap (rad ; + = montée devant → freine l'avance).
inline float terrainSlopeAlong(float x, float y, float heading)
{
    constexpr float EPS = 0.5f;
    const float cx = std::cos(heading), sy = std::sin(heading);
    const float ahead  = terrainH(x + EPS * cx, y + EPS * sy);
    const float behind = terrainH(x - EPS * cx, y - EPS * sy);
    return std::atan((ahead - behind) / (2.f * EPS));
}

} // namespace sim
