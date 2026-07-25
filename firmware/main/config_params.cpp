// config_params.cpp — Parameter table + defaults/bounds. PURE (also compiled on
// the host for simulation): the single source of truth for the settings, without NVS or ESP.
#include "control_types.hpp"

#include <algorithm>

// Single source of truth: name (NVS/JSON key), label, category (visual grouping),
// help (hover tooltip — WITHOUT double quotes, injected as-is into the JSON),
// type, min/default/max, target field. The array order = the display order: keep the
// entries of a same category CONSECUTIVE (the web page groups identical cats that follow each other).
// NB: the LVC thresholds are NOT here — 12 V or 24 V battery detected at startup,
// thresholds hard-coded per type (hw::VBAT12_*/VBAT24_*).
const ParamDesc PARAMS[] =
{
    {"speed_limit_ms",  "Speed limit (m/s)",   "Speed & power",
     "Maximum speed in FORWARD (m/s). Beyond it, the PID limiter holds the kart back (encoders required). It is also the speed at which the turn is most limited by the rollover protection.",
     PType::Float, 0.3f,  3.3f,   7.f,    &KartConfig::speed_limit_ms},
    {"rev_speed_ms",    "Reverse limit (m/s)",     "Speed & power",
     "Maximum speed in REVERSE (m/s) — same PID limiter, target chosen according to the MEASURED direction. The caster wheel does not steer in reverse: stay slow.",
     PType::Float, 0.3f,  1.0f,   3.f,    &KartConfig::rev_speed_ms},
    // MANUAL PWM cap (the most restrictive wins) — the AUTOMATIC 12 V/Vbat
    // measured cap (ctl::dutyCapVolts) applies IN ADDITION. Default 1.0 = let the auto handle it.
    {"duty_cap",        "Manual PWM cap (0-1)", "Speed & power",
     "Fixed PWM cap, in addition to the automatic cap 12 V / measured battery voltage (the most restrictive wins). Leave at 1.0 in normal use; lower it by hand if driving without a voltage sensor above 12 V.",
     PType::Float, 0.05f, 1.0f,   1.f,    &KartConfig::duty_cap_frac},
    {"thr_deadzone",    "Stick deadzone",       "Gamepad",
     "Radius around the stick center where the position is ignored (0-0.3). Compensates for a stick that does not return exactly to center.",
     PType::Float, 0.f,   0.06f,  0.30f,  &KartConfig::thr_deadzone},
    {"thr_ramp_per_s",  "Forward smoothness (D/s)",    "Gamepad",
     "Maximum slope of the forward command (full scale per second). Smaller = gentler starts and stops.",
     PType::Float, 0.2f,  2.f,    20.f,   &KartConfig::thr_ramp_per_s},
    {"turn_gain",       "Turn gain (0-1)",    "Gamepad",
     "Share of the left/right differential at full stick. 1 = pivot in place at full power.",
     PType::Float, 0.f,   1.f,    1.f,    &KartConfig::turn_gain},
    {"turn_rate",       "Turn smoothness (D/s)",    "Gamepad",
     "Maximum slope of the turn command (full scale per second). Smooths abrupt stick moves.",
     PType::Float, 0.3f,  3.0f,   20.f,   &KartConfig::turn_rate},
    // "iso-a_lat" rollover protection: turn ±100% below turn_full_ms (and everywhere
    // 1/v allows it), then limit ∝ 1/v up to turn_hi at speed_limit_ms (MEASURED speed),
    // and it keeps tightening beyond (runaway).
    {"turn_limit_en",   "Rollover protection (0/1)", "Rollover protection",
     "1 = the turn limit follows the measured speed (rollover protection ramp). 0 = disabled — for bench testing only, turn at 100% at all speeds.",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::turn_limit_en},
    {"turn_full_ms",    "Turn 100% below (m/s)",  "Rollover protection",
     "Below this vehicle speed (m/s), the turn is allowed at 100% (pivot in place permitted). Beyond it, the limit decreases linearly down to the Vmax limit.",
     PType::Float, 0.1f,  0.5f,   0.8f,   &KartConfig::turn_full_ms},
    // Default 0.2 + 1/v curve (turn gain 1.0): the physical simulation shows that an
    // OFFSET LOAD (child alone on one side, adult+child) tips over with the old
    // linear ramp as soon as gain=1 — the iso-a_lat at 0.2 restores healthy margins (≥ +0.8 m/s²).
    {"turn_alat_vmax",  "Max turn at Vmax (0-1)", "Rollover protection",
     "Turn limit at maximum speed; in between, the limit follows 1/v (same lateral acceleration at all speeds). 0.2 = the only value verified safe by simulation for offset loads (child alone on one side, adult+child) with turn gain 1.",
     PType::Float, 0.1f,  0.2f,   0.4f,   &KartConfig::turn_hi},
    {"vbat_div_ratio",  "Vbat divider ratio",     "Battery",
     "Ratio of the voltage-measurement divider bridge: Vbat = voltage read by the ADS1115 x this ratio. To calibrate with a multimeter. The battery type (12 or 24 V) and the cutoff thresholds are detected automatically at startup.",
     PType::Float, 1.f,   7.667f, 20.f,   &KartConfig::vbat_div_ratio},
    // PID in m/s (the error is in m/s since the km/h→m/s switch).
    {"brk_kp",          "PID brake Kp (m/s)",      "Control loops (PID)",
     "Proportional gain of the active electric brake (speed command 0). Encoders required.",
     PType::Float, 0.f,   0.43f,  5.f,    &KartConfig::brk_kp},
    {"brk_ki",          "PID brake Ki (m/s)",      "Control loops (PID)",
     "Integral gain of the active electric brake: catches up a slope that makes the kart slide when stopped.",
     PType::Float, 0.f,   0.29f,  10.f,   &KartConfig::brk_ki},
    {"brk_kd",          "PID brake Kd (m/s)",      "Control loops (PID)",
     "Derivative gain of the active electric brake: damps braking oscillations.",
     PType::Float, 0.f,   0.011f, 2.f,    &KartConfig::brk_kd},
    {"vlim_enable",     "Speed limiter (0/1)",  "Control loops (PID)",
     "1 = the limiter PID holds the kart back at the speed limit (encoders required). 0 = disabled — only the PWM cap bounds the speed.",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::vlim_enable},
    {"vlim_kp",         "PID limiter Kp (m/s)",   "Control loops (PID)",
     "Proportional gain of the speed limiter (holds the kart back at the speed limit).",
     PType::Float, 0.f,   0.54f,  5.f,    &KartConfig::vlim_kp},
    {"vlim_ki",         "PID limiter Ki (m/s)",   "Control loops (PID)",
     "Integral gain of the speed limiter: holds the limit downhill.",
     PType::Float, 0.f,   0.50f,  10.f,   &KartConfig::vlim_ki},
    {"vlim_kd",         "PID limiter Kd (m/s)",   "Control loops (PID)",
     "Derivative gain of the speed limiter: damps the oscillations around the limit.",
     PType::Float, 0.f,   0.f,    2.f,    &KartConfig::vlim_kd},
    {"brk_pid_enable",  "PID brake (0/1)",         "Control loops (PID)",
     "1 = active electric brake (PID, speed command 0) when the stick is released (encoders required). 0 = dynamic braking only (motor short-circuit).",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::brk_pid_enable},
    {"use_encoders",    "Use encoders (0/1)", "Behavior",
     "1 = the AS5600 sensors are used for the speed limiter, PID braking, rollover protection and the blocking sensor fault. 0 = test bench without wired encoders.",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::use_encoders},
    {"allow_reverse",   "Reverse (0/1)",    "Behavior",
     "Allow reverse (held by its own speed limit, rev_speed_ms).",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::allow_reverse},
    {"arm_hold_ms",     "Arming hold (ms)",     "Behavior",
     "Held press duration on START (physical or gamepad) to arm, centered stick required.",
     PType::Int,   200.f, 1000.f, 5000.f, &KartConfig::arm_hold_ms},
    {"disarm_s",        "Auto disarm (s)",    "Behavior",
     "Automatic disarm after this delay without touching the stick.",
     PType::Int,   5.f,   30.f,   600.f,  &KartConfig::disarm_s},
    {"led_count",       "Strip LED count",           "LEDs",
     "Number of WS2812 LEDs on the status strip.",
     PType::Int,   1.f,   10.f,   60.f,   &KartConfig::led_count},
    {"led_brightness",  "LED brightness",         "LEDs",
     "Strip brightness (1-255).",
     PType::Int,   1.f,   64.f,   255.f,  &KartConfig::led_brightness},
};
const int PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

void KartConfig::setDefaults()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        this->*(PARAMS[i].field) = PARAMS[i].def;
    }
    // Encoder ratios (outside PARAMS — see control_types.hpp): 10" wheel via AS5600 + reduction gear.
    enc_mps_per_cps = 3.14159265f * hw::WHEEL_DIAM_M / (hw::AS5600_CPR * hw::GEAR_RATIO);
    enc_rpm_per_cps = 60.f / hw::AS5600_CPR;   // ENCODER-SHAFT rpm (raw, ratio-independent)
}

void KartConfig::clampAll()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        float& f = this->*(PARAMS[i].field);
        f = std::clamp(f, PARAMS[i].min, PARAMS[i].max);
    }
    // (The LVC thresholds are no longer parameters: hard-coded according to the battery
    // 12/24 V detected at startup — hw::VBAT12_*/VBAT24_*, consistency guaranteed.)
}
