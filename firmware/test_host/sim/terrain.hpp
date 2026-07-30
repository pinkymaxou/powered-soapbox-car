// terrain.hpp — Hilly terrain of the MANUAL DRIVING mode: sum of Gaussian hills.
// The slope under the kart (along the heading) feeds the physics at each step — climbing slows down,
// descending runs away, exactly like the slope scenarios, but continuously.
// ⚠️ JS MIRROR: the same hills are coded in tools/sim_viewer.html (3D mesh) —
// keep the two tables synchronized.
#pragma once

#include <cmath>

namespace sim
{

struct Hill
{
    float cx, cy;    // center (m)
    float sigma;     // spread (m)
    float amp;       // height (m)
};

// Starting zone (±20 m around the origin) nearly flat; max slopes ~13% on the flanks —
// some therefore exceed the holding capacity of the motors (~11%): realistic and instructive.
constexpr Hill TERRAIN_HILLS[] = {
    {  60.f,  20.f, 18.f, 4.0f },
    { -50.f,  40.f, 22.f, 5.0f },
    {  30.f, -70.f, 25.f, 6.0f },
    { -70.f, -60.f, 20.f, 3.5f },
    {  90.f, -30.f, 16.f, 3.0f },
};

// RAMP: 0.6 m rise over 3 m (slope ~20%) with a SHARP EDGE at x=17 — crossed at
// full speed, the kart TAKES OFF (the vertical physics is ballistic once airborne).
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

// Terrain slope under the kart, along the heading (rad; + = uphill ahead → brakes the advance).
inline float terrainSlopeAlong(float x, float y, float heading)
{
    constexpr float EPS = 0.5f;
    const float cx = std::cos(heading), sy = std::sin(heading);
    const float ahead  = terrainH(x + EPS * cx, y + EPS * sy);
    const float behind = terrainH(x - EPS * cx, y - EPS * sy);
    return std::atan((ahead - behind) / (2.f * EPS));
}

// ─────────────────────────── The shed 🚪 ───────────────────────────
// A hut standing in the start corridor. Drive through its door and the floor is not there.
// MANUAL DRIVING ONLY (sim_main --drive): the CI scenarios never load the terrain, so none of
// this can perturb them. ⚠️ JS MIRROR in tools/sim_viewer.html — keep both in step.
constexpr float SHED_CX = 24.f, SHED_CY = -6.f;   // centre (m), clear of the ramp at y≈0
constexpr float SHED_HALF = 2.0f;                 // 4 m × 4 m footprint
constexpr float SHED_WALL = 0.15f;
constexpr float SHED_H = 2.4f;
constexpr float BACKROOMS_FLOOR = -8.f;           // you fall this far. Mind the step.
constexpr float BACKROOMS_ROLL_N = 95.f;          // damp carpet: ~3x the rolling resistance

// Inside the four walls, i.e. past the doorway (the door is the whole -x face).
inline bool inShed(float x, float y)
{
    const float ix = SHED_HALF - SHED_WALL;
    return (x > SHED_CX - ix) && (x < SHED_CX + ix) &&
           (y > SHED_CY - ix) && (y < SHED_CY + ix);
}

} // namespace sim
