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
// Damp carpet. 95 N left only ~9 N of the ~104 N of drive force — the kart crawled.
// 50 N still reads as "heavy carpet" (~0.55 m/s² instead of ~0.9) but stays drivable.
constexpr float BACKROOMS_ROLL_N = 50.f;

// Inside the four walls, i.e. past the doorway (the door is the whole -x face).
inline bool inShed(float x, float y)
{
    const float ix = SHED_HALF - SHED_WALL;
    return (x > SHED_CX - ix) && (x < SHED_CX + ix) &&
           (y > SHED_CY - ix) && (y < SHED_CY + ix);
}

// The shed's three solid walls (the -x face is the door). Collision only — before this,
// the walls were viewer decor and you could fall into the Backrooms by clipping through
// the BACK wall, which rather undermined the one-way-door gag.
inline bool shedWallAt(float x, float y)
{
    const float dx = x - SHED_CX, dy = y - SHED_CY;
    const float out = SHED_HALF + 0.25f;               // + kart body margin
    const float in  = SHED_HALF - SHED_WALL - 0.25f;
    if (std::fabs(dx) > out || std::fabs(dy) > out) return false;   // clear of the shed
    if (std::fabs(dx) < in && std::fabs(dy) < in) return false;     // inside the room
    return dx > -in;   // in the wall ring — except the -x band, which is the doorway
}

// ─────────────────────── Backrooms walls 🧱 (collision) ───────────────────────
// Same PROCEDURAL layout as the viewer draws — ⚠️ JS MIRROR in tools/sim_viewer.html,
// identical hash and constants; change one side, change the other. Grid of S-metre cell
// lines; each line segment carries a wall or not from a hash of its indices. The JS only
// RENDERS them; this is what makes them SOLID (Vehicle::wall_fn — walls you could drive
// straight through broke the illusion). Both sides use int32 arithmetic: |i|,|j| ≤ N keeps
// the products far from overflow, and the JS ^ operator coerces to int32 exactly the same.
constexpr float BR_GRID_S    = 8.f;     // cell size (m)
constexpr int   BR_GRID_N    = 7;       // segments exist for indices in [-N..N]
constexpr float BR_WALL_HALF = 0.41f;   // collision half-thickness: 0.11 visual + kart margin

inline uint32_t brHash(int i, int j)
{
    return static_cast<uint32_t>((i * 73856093) ^ (j * 19349663));
}
inline bool brWallX(int i, int j)   // segment along X: x ∈ [iS, (i+1)S], y = jS
{
    return brHash(i, j) % 10u < 6u;
}
inline bool brWallZ(int i, int j)   // segment along Y: x = iS, y ∈ [jS, (j+1)S]
{
    return (brHash(i, j) >> 4) % 10u < 6u;
}

inline bool backroomsWallAt(float x, float y)
{
    const int i0 = static_cast<int>(std::floor(x / BR_GRID_S));
    const int j0 = static_cast<int>(std::floor(y / BR_GRID_S));
    // The point sits in cell (i0, j0): test the two X-walls on its north/south lines and
    // the two Z-walls on its west/east lines — never more than four candidates.
    for (int j = j0; j <= j0 + 1; ++j)
    {
        if (i0 < -BR_GRID_N || i0 > BR_GRID_N || j < -BR_GRID_N || j > BR_GRID_N) continue;
        if (brWallX(i0, j) && std::fabs(y - j * BR_GRID_S) < BR_WALL_HALF) return true;
    }
    for (int i = i0; i <= i0 + 1; ++i)
    {
        if (i < -BR_GRID_N || i > BR_GRID_N || j0 < -BR_GRID_N || j0 > BR_GRID_N) continue;
        if (brWallZ(i, j0) && std::fabs(x - i * BR_GRID_S) < BR_WALL_HALF) return true;
    }
    return false;
}

} // namespace sim
