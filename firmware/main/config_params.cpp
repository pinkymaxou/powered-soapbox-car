// config_params.cpp — Parameter table + defaults/bounds. PURE (also compiled on
// the host for simulation): the single source of truth for the settings, without NVS or ESP.
#include "control_types.hpp"

#include <algorithm>
#include <cmath>

// Single source of truth: name (NVS/JSON key), label, category (visual grouping),
// help (hover tooltip — WITHOUT double quotes, injected as-is into the JSON),
// type, min/default/max, target field. The array order = the display order: keep the
// entries of a same category CONSECUTIVE (the web page groups identical cats that follow each other).
// Each value/field is TYPED: {.f=…} for Float params, {.i=…} for Int/Bool (see CfgVal/CfgField).
// NB: nothing about the battery lives here any more — the pack is always 12 V, it is not
// measured, and the firmware cannot cut its own power (single main relay, see doc/electronique.md).
// Nor anything about speed, braking loops or rollover (2026-09-29): there is no sensor on the
// wheels, so the kart is stick → PWM and duty_cap is the only thing that bounds it.
extern constexpr ParamDesc PARAMS[] =
{
    // PWM cap — the ONLY limit left. Nothing measures the speed, nothing limits the turn: this
    // number decides how fast the kart goes AND whether a full turn at that speed stays upright.
    {"duty_cap",        "PWM cap (0-1)", "Speed & power",
     "PWM ceiling for both motors - the ONLY speed limit (no encoders, no rollover protection). In simulation (18T-32T chain) a full turn at full speed TIPS the kart at 1.0 with a child sitting to one side or an adult aboard; two children centred stay upright. 0.9 keeps all three load cases upright, 0.8 gives the margin the old protection had.",
     PType::Float, {.f = 0.05f}, {.f = 1.0f}, {.f = 1.f}, {.f = &KartConfig::duty_cap_frac}},
    {"thr_deadzone",    "Stick deadzone",       "Gamepad",
     "Radius around the stick center where the position is ignored (0-0.3). Inside it, with A held, the kart freewheels.",
     PType::Float, {.f = 0.f}, {.f = 0.06f}, {.f = 0.30f}, {.f = &KartConfig::thr_deadzone}},
    {"turn_gain",       "Turn gain (0-1)",    "Gamepad",
     "Share of the left/right differential at full stick. 1 = pivot in place at full power.",
     PType::Float, {.f = 0.f}, {.f = 1.f}, {.f = 1.f}, {.f = &KartConfig::turn_gain}},
    // Pluggable stick→motor mixing (mixer.hpp). The PWM cap applies outside the mixer.
    {"mix_type",        "Mixing type (0/1)",     "Drive feel",
     "How the stick maps to the motors. 0 = linear. 1 = expo: gentle around center, full authority at the stops (recommended for a child).",
     PType::Int,   {.i = 0}, {.i = 0}, {.i = 1}, {.i = &KartConfig::mix_type}},
    {"mix_expo_fwd",    "Expo throttle (0-1)",     "Drive feel",
     "Exponential strength on the throttle axis (mixing type 1). 0 = linear, 1 = cubic (softest mid-range). Half-stick gives 31% drive at 0.5 instead of 50%.",
     PType::Float, {.f = 0.f}, {.f = 0.5f}, {.f = 1.f}, {.f = &KartConfig::mix_expo_fwd}},
    {"mix_expo_turn",   "Expo steering (0-1)",     "Drive feel",
     "Exponential strength on the steering axis (mixing type 1). Tames twitchy steering around center without giving up the full pivot at the stops.",
     PType::Float, {.f = 0.f}, {.f = 0.6f}, {.f = 1.f}, {.f = &KartConfig::mix_expo_turn}},
    // ONE pair of slopes on the motor command, both in % of full duty per second, because they
    // turned out to be the same idea in two units: the old `coast_s`/`coast_ms` freewheel was
    // already nothing but a downward slope. Rates are in REAL duty, so lowering duty_cap does
    // not secretly retune them. Neither of them can delay the BRAKE: releasing A, disarming or
    // any Stop cause grounds the windings on the spot, whatever these say.
    {"accel_pct_s",     "Accel ramp (%/s)",        "Drive feel",
     "How fast the PWM may RISE, in % of full duty per second: 100 = a full second from standstill to full power, 200 = half a second. Spares the motor drivers and the chain the current spike of a stick slammed open, and makes the kart pull away smoothly. 0 = no limit, the stick goes straight through.",
     PType::Int,   {.i = 0}, {.i = 200}, {.i = 1000}, {.i = &KartConfig::accel_pct_s}},
    // The MDD20A has NO coast state (PWM low = both outputs grounded = brake), so a freewheel
    // cannot be had: it is imitated by letting the command slide to 0 instead of dropping it.
    // That slide is this rate. Flat ground only — nothing measures a slope.
    {"decel_pct_s",     "Decel ramp (%/s)",        "Drive feel",
     "How fast the PWM may FALL back toward 0, same units. This is the freewheel: with A held and the stick centred the command slides to 0 at this rate instead of dropping (100 = one second from full power, 50 = two seconds), so the motor neither pushes nor brakes much on the way. It also smooths a stick pulled back. 0 = no limit: the command drops at once, which on this driver IS the brake.",
     PType::Int,   {.i = 0}, {.i = 100}, {.i = 1000}, {.i = &KartConfig::decel_pct_s}},
    {"mot_inv_l",       "Invert LEFT motor",      "Behavior",
     "Flips the LEFT motor direction (= swapping its two leads). Set it if the left wheel turns backwards on forward stick (wheels in the air).",
     PType::Bool,  {.i = 0}, {.i = 0}, {.i = 1}, {.i = &KartConfig::mot_inv_l}},
    {"mot_inv_r",       "Invert RIGHT motor",     "Behavior",
     "Same for the RIGHT motor.",
     PType::Bool,  {.i = 0}, {.i = 0}, {.i = 1}, {.i = &KartConfig::mot_inv_r}},
    {"mot_swap_lr",     "Swap LEFT/RIGHT motors", "Behavior",
     "1 = the motors are on crossed driver channels: forward is right but the steering is mirrored. The inversions above still name the WHEEL.",
     PType::Bool,  {.i = 0}, {.i = 0}, {.i = 1}, {.i = &KartConfig::mot_swap_lr}},
    {"arm_hold_ms",     "Arming hold (ms)",     "Behavior",
     "Held press duration on the gamepad's START/Options button to arm, centered stick required. Once armed, the kart only moves while A is held.",
     PType::Int,   {.i = 200}, {.i = 1000}, {.i = 5000}, {.i = &KartConfig::arm_hold_ms}},
    {"disarm_s",        "Auto disarm (s)",    "Behavior",
     "Automatic disarm after this delay without driving (A held + stick).",
     PType::Int,   {.i = 5}, {.i = 30}, {.i = 600}, {.i = &KartConfig::disarm_s}},
    {"led_count",       "Strip LED count",           "LEDs",
     "Number of WS2812 LEDs on the status strip.",
     PType::Int,   {.i = 1}, {.i = 10}, {.i = 60}, {.i = &KartConfig::led_count}},
    {"led_brightness",  "LED brightness",         "LEDs",
     "Strip brightness (1-255).",
     PType::Int,   {.i = 1}, {.i = 64}, {.i = 255}, {.i = &KartConfig::led_brightness}},
};
const int PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

// ── Typed access (widen to / narrow from float; the config wire stays float) ──
float cfgGet(const KartConfig& c, const ParamDesc& p)
{
    return (PType::Float == p.type) ? c.*(p.field.f) : static_cast<float>(c.*(p.field.i));
}
void cfgSet(KartConfig& c, const ParamDesc& p, float v)
{
    if (PType::Float == p.type) c.*(p.field.f) = v;
    else                        c.*(p.field.i) = static_cast<int32_t>(std::lround(v));
}
float cfgMin(const ParamDesc& p) { return (PType::Float == p.type) ? p.min.f : static_cast<float>(p.min.i); }
float cfgMax(const ParamDesc& p) { return (PType::Float == p.type) ? p.max.f : static_cast<float>(p.max.i); }
float cfgDef(const ParamDesc& p) { return (PType::Float == p.type) ? p.def.f : static_cast<float>(p.def.i); }

void KartConfig::setDefaults()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        cfgSet(*this, PARAMS[i], cfgDef(PARAMS[i]));
    }
}

void KartConfig::clampAll()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        const ParamDesc& p = PARAMS[i];
        cfgSet(*this, p, std::clamp(cfgGet(*this, p), cfgMin(p), cfgMax(p)));
    }
}

// ───────────── Compile-time guard on the protobuf reply buffer ─────────────
// The webserver encodes every reply into one static arena of hw::PB_REPLY_CAP bytes. The
// biggest reply BY FAR is the config: PARAMS, with four strings per entry. It is encoded
// through nanopb CALLBACKS, so nanopb cannot emit a *_size for it — but this file can, since
// PARAMS is right here. Bound it and fail the BUILD, rather than discovering at runtime that
// `get` returns nothing and the page's socket dies (which is exactly what happened when four
// params with long help text pushed past the old 6144-byte arena).
namespace
{
constexpr size_t cstrLen(const char* s)
{
    size_t n = 0;
    while ('\0' != s[n]) ++n;
    return n;
}

// Worst case for one ParamMeta inside Config, protobuf wire format:
//   submessage header  = tag(1) + length varint(2, messages stay < 16 kB)
//   each of the 4 strings = tag(1) + length varint(2) + the bytes
//   each of the 4 typed oneofs = tag(2, fields go up to 19) + payload(5, int32 varint worst case)
constexpr size_t paramPbBytes(const ParamDesc& p)
{
    return 3 + 4 * 3 + cstrLen(p.name) + cstrLen(p.desc) + cstrLen(p.cat) + cstrLen(p.help)
             + 4 * 7;
}

constexpr size_t configPbBytes()
{
    size_t n = 0;
    for (const ParamDesc& p : PARAMS) n += paramPbBytes(p);
    return n;
}

// Vals: repeated ParamVal, one per parameter. ParamVal_size comes from nanopb (28 B), plus
// the submessage header. Hard-coded here to keep this file free of the generated header.
constexpr size_t valsPbBytes() { return (sizeof(PARAMS) / sizeof(PARAMS[0])) * (3 + 28); }
} // namespace

static_assert(configPbBytes() <= hw::PB_REPLY_CAP,
              "Config no longer fits in hw::PB_REPLY_CAP. Adding a parameter grew it past the "
              "arena: raise PB_REPLY_CAP in control_types.hpp (and mind the static RAM), or "
              "shorten some help texts. Do NOT ignore this — at runtime the encode fails "
              "silently and the web page loses its WebSocket on load.");
static_assert(valsPbBytes() <= hw::PB_REPLY_CAP, "Vals no longer fits in hw::PB_REPLY_CAP.");
