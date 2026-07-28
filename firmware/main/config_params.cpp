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
// NB: the LVC thresholds are NOT here — 12 V or 24 V battery detected at startup,
// thresholds hard-coded per type (hw::VBAT12_*/VBAT24_*).
const ParamDesc PARAMS[] =
{
    {"speed_limit_ms",  "Speed limit (m/s)",   "Speed & power",
     "Maximum speed in FORWARD (m/s). Beyond it, the PID limiter holds the kart back (encoders required). It is also the speed at which the turn is most limited by the rollover protection.",
     PType::Float, {.f = 0.3f}, {.f = 3.3f}, {.f = 7.f}, {.f = &KartConfig::speed_limit_ms}},
    {"rev_speed_ms",    "Reverse limit (m/s)",     "Speed & power",
     "Maximum speed in REVERSE (m/s) — same PID limiter, target chosen according to the MEASURED direction. The caster wheel does not steer in reverse: stay slow.",
     PType::Float, {.f = 0.3f}, {.f = 1.0f}, {.f = 3.f}, {.f = &KartConfig::rev_speed_ms}},
    // MANUAL PWM cap (the most restrictive wins) — the AUTOMATIC 12 V/Vbat
    // measured cap (ctl::dutyCapVolts) applies IN ADDITION. Default 1.0 = let the auto handle it.
    {"duty_cap",        "Manual PWM cap (0-1)", "Speed & power",
     "Fixed PWM cap, in addition to the automatic cap 12 V / measured battery voltage (the most restrictive wins). Leave at 1.0 in normal use; lower it by hand if driving without a voltage sensor above 12 V.",
     PType::Float, {.f = 0.05f}, {.f = 1.0f}, {.f = 1.f}, {.f = &KartConfig::duty_cap_frac}},
    {"thr_deadzone",    "Stick deadzone",       "Gamepad",
     "Radius around the stick center where the position is ignored (0-0.3). Compensates for a stick that does not return exactly to center.",
     PType::Float, {.f = 0.f}, {.f = 0.06f}, {.f = 0.30f}, {.f = &KartConfig::thr_deadzone}},
    {"turn_gain",       "Turn gain (0-1)",    "Gamepad",
     "Share of the left/right differential at full stick. 1 = pivot in place at full power.",
     PType::Float, {.f = 0.f}, {.f = 1.f}, {.f = 1.f}, {.f = &KartConfig::turn_gain}},
    {"turn_rate",       "Turn smoothness (D/s)",    "Gamepad",
     "Maximum slope of the turn command (full scale per second). Smooths abrupt stick moves.",
     PType::Float, {.f = 0.3f}, {.f = 3.0f}, {.f = 20.f}, {.f = &KartConfig::turn_rate}},
    // "iso-a_lat" rollover protection: turn ±100% below turn_full_ms (and everywhere
    // 1/v allows it), then limit ∝ 1/v up to turn_hi at speed_limit_ms (MEASURED speed),
    // and it keeps tightening beyond (runaway).
    {"turn_limit_en",   "Rollover protection (0/1)", "Rollover protection",
     "1 = the turn limit follows the measured speed (rollover protection ramp). 0 = disabled — for bench testing only, turn at 100% at all speeds.",
     PType::Bool,  {.i = 0}, {.i = 1}, {.i = 1}, {.i = &KartConfig::turn_limit_en}},
    {"turn_full_ms",    "Turn 100% below (m/s)",  "Rollover protection",
     "Below this vehicle speed (m/s), the turn is allowed at 100% (pivot in place permitted). Beyond it, the limit decreases linearly down to the Vmax limit.",
     PType::Float, {.f = 0.1f}, {.f = 0.5f}, {.f = 0.8f}, {.f = &KartConfig::turn_full_ms}},
    // Default 0.2 + 1/v curve (turn gain 1.0): the physical simulation shows that an
    // OFFSET LOAD (child alone on one side, adult+child) tips over with the old
    // linear ramp as soon as gain=1 — the iso-a_lat at 0.2 restores healthy margins (≥ +0.8 m/s²).
    {"turn_alat_vmax",  "Max turn at Vmax (0-1)", "Rollover protection",
     "Turn limit at maximum speed; in between, the limit follows 1/v (same lateral acceleration at all speeds). 0.2 = the only value verified safe by simulation for offset loads (child alone on one side, adult+child) with turn gain 1.",
     PType::Float, {.f = 0.1f}, {.f = 0.2f}, {.f = 0.4f}, {.f = &KartConfig::turn_hi}},
    // (No vbat_div_ratio: the divider is fixed by the resistors on the board — hw::VBAT_DIV_RATIO.)
    {"vbat_check_en",   "Voltage check (0/1)",    "Battery",
     "1 = the low-voltage cutoff (LVC) is a driving condition: below the threshold of the detected battery the kart disarms, refuses to move, and cuts the power after 30 s. 0 = the voltage is still measured, displayed and graphed, but NEVER blocks anything — for bench work with no pack on the divider bridge. WARNING: with 0 the battery's own BMS is the only remaining protection against deep discharge.",
     PType::Bool,  {.i = 0}, {.i = 1}, {.i = 1}, {.i = &KartConfig::vbat_check_en}},
    // PID gains operate on an error in m/s.
    {"brk_kp",          "PID brake Kp (m/s)",      "Control loops (PID)",
     "Proportional gain of the active electric brake (speed command 0). Encoders required.",
     PType::Float, {.f = 0.f}, {.f = 0.43f}, {.f = 5.f}, {.f = &KartConfig::brk_kp}},
    {"brk_ki",          "PID brake Ki (m/s)",      "Control loops (PID)",
     "Integral gain of the active electric brake: catches up a slope that makes the kart slide when stopped.",
     PType::Float, {.f = 0.f}, {.f = 0.29f}, {.f = 10.f}, {.f = &KartConfig::brk_ki}},
    {"brk_kd",          "PID brake Kd (m/s)",      "Control loops (PID)",
     "Derivative gain of the active electric brake: damps braking oscillations.",
     PType::Float, {.f = 0.f}, {.f = 0.011f}, {.f = 2.f}, {.f = &KartConfig::brk_kd}},
    {"vlim_enable",     "Speed limiter (0/1)",  "Control loops (PID)",
     "1 = the limiter PID holds the kart back at the speed limit (encoders required). 0 = disabled — only the PWM cap bounds the speed.",
     PType::Bool,  {.i = 0}, {.i = 1}, {.i = 1}, {.i = &KartConfig::vlim_enable}},
    {"vlim_kp",         "PID limiter Kp (m/s)",   "Control loops (PID)",
     "Proportional gain of the speed limiter (holds the kart back at the speed limit).",
     PType::Float, {.f = 0.f}, {.f = 0.54f}, {.f = 5.f}, {.f = &KartConfig::vlim_kp}},
    {"vlim_ki",         "PID limiter Ki (m/s)",   "Control loops (PID)",
     "Integral gain of the speed limiter: holds the limit downhill.",
     PType::Float, {.f = 0.f}, {.f = 0.50f}, {.f = 10.f}, {.f = &KartConfig::vlim_ki}},
    {"vlim_kd",         "PID limiter Kd (m/s)",   "Control loops (PID)",
     "Derivative gain of the speed limiter: damps the oscillations around the limit.",
     PType::Float, {.f = 0.f}, {.f = 0.f}, {.f = 2.f}, {.f = &KartConfig::vlim_kd}},
    {"brk_pid_enable",  "PID brake (0/1)",         "Control loops (PID)",
     "1 = active electric brake (PID, speed command 0) when the stick is released (encoders required). 0 = dynamic braking only (motor short-circuit).",
     PType::Bool,  {.i = 0}, {.i = 1}, {.i = 1}, {.i = &KartConfig::brk_pid_enable}},
    {"dyn_brake_en",    "Dynamic brake (0/1)", "Behavior",
     "1 = short-circuit the motors to hold the kart when the stick is released (armed). 0 = FREEWHEEL — coast instead of braking on release. The safety brake when disarmed / faulted / e-stopped is ALWAYS on regardless of this setting.",
     PType::Bool,  {.i = 0}, {.i = 1}, {.i = 1}, {.i = &KartConfig::dyn_brake_en}},
    {"open_loop",       "Open-loop test mode (0/1)", "Behavior",
     "1 = TEST MODE: the mixed stick command goes STRAIGHT to the motors — no speed limiter, no rollover protection, no PID braking, no smoothing. The PWM cap and every safety gate (arming, heartbeat, e-stop, blocking faults) still apply. Use on a stand / with caution.",
     PType::Bool,  {.i = 0}, {.i = 0}, {.i = 1}, {.i = &KartConfig::open_loop}},
    {"use_encoders",    "Use encoders (0/1)", "Behavior",
     "1 = the AS5600 sensors are used for the speed limiter, PID braking, rollover protection and the blocking sensor fault. 0 = test bench without wired encoders.",
     PType::Bool,  {.i = 0}, {.i = 1}, {.i = 1}, {.i = &KartConfig::use_encoders}},
    {"enc_per_wheel",   "Enc turns / wheel turn", "Behavior",
     "Encoder-shaft turns per wheel turn — set it to match where the magnet sits: gearbox output = 1.28, 1:5 intermediate shaft = 3.41. Converts encoder counts to WHEEL speed (limiter, rollover protection, sanity check). The raw rpm readout is unaffected.",
     PType::Float, {.f = 1.f}, {.f = 1.28f}, {.f = 10.f}, {.f = &KartConfig::enc_per_wheel}},
    // (No allow_reverse: reverse is ALWAYS permitted, held by its own limit rev_speed_ms.)
    {"arm_hold_ms",     "Arming hold (ms)",     "Behavior",
     "Held press duration on START (physical or gamepad) to arm, centered stick required.",
     PType::Int,   {.i = 200}, {.i = 1000}, {.i = 5000}, {.i = &KartConfig::arm_hold_ms}},
    {"disarm_s",        "Auto disarm (s)",    "Behavior",
     "Automatic disarm after this delay without touching the stick.",
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
    // Derived encoder ratios (see control_types.hpp). enc_mps_per_cps depends on the
    // enc_per_wheel mount ratio (set by the loop above); enc_rpm_per_cps is the raw shaft rpm.
    enc_mps_per_cps = 3.14159265f * hw::WHEEL_DIAM_M / (hw::AS5600_CPR * enc_per_wheel);
    enc_rpm_per_cps = 60.f / hw::AS5600_CPR;   // ENCODER-SHAFT rpm (raw, ratio-independent)
}

void KartConfig::clampAll()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        const ParamDesc& p = PARAMS[i];
        cfgSet(*this, p, std::clamp(cfgGet(*this, p), cfgMin(p), cfgMax(p)));
    }
    // enc_mps_per_cps depends on enc_per_wheel — recompute after any change so the wheel
    // speed (limiter / rollover / sanity) matches the encoder mount.
    enc_mps_per_cps = 3.14159265f * hw::WHEEL_DIAM_M / (hw::AS5600_CPR * enc_per_wheel);
    // (The LVC thresholds are no longer parameters: hard-coded according to the battery
    // 12/24 V detected at startup — hw::VBAT12_*/VBAT24_*, consistency guaranteed.)
}
