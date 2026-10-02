// control_types.hpp — PURE control types and constants (compilable on the host).
// Extracted from config.hpp so the control logic (controller_core) and the simulator
// (test_host/sim) share the SAME source of truth: KartConfig, the state/stop enums
// and hw:: constants — without any ESP-IDF dependency.
#pragma once

#include <cstddef>
#include <cstdint>

// Round a float to the nearest int (half away from zero).
inline int iround(float v)
{
    return static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f);
}

// ───────────────────────── Hardware constants (compile-time) ─────────────────────────
namespace hw
{
constexpr int   CTRL_HZ       = 500;   // control loop (FreeRTOS tick 1000 Hz)
constexpr int   CTRL_DT_MS    = 1000 / CTRL_HZ;
constexpr float CTRL_DT_S     = 1.0f / CTRL_HZ;
constexpr int   WDT_TIMEOUT_S = 2;   // mirror of sdkconfig's CONFIG_ESP_TASK_WDT_TIMEOUT_S
                                     // (the sdkconfig value is what actually arms it, with
                                     // PANIC=y: a stalled control loop reboots, not warns)

constexpr int PWM_FREQ_HZ = 18000;
constexpr int PWM_MAX     = 4095;   // 12 bits (the LEDC resolution lives in hardware.cpp)

// Motors nominally 12 V, driver 6–30 V. The pack is ALWAYS 12 V (design input, see
// doc/electronique.md): nothing measures the voltage any more, so the only PWM ceiling
// is the manual one (KartConfig::duty_cap_frac). The old automatic cap 12 V / measured
// Vbat, the LVC and their smoothing constants went with the ADS1115.

// Protobuf reply arena (webserver.cpp). Declared HERE, not there, so config_params.cpp —
// the file that grows every time a parameter is added — can static_assert against it and
// FAIL THE BUILD instead of failing the socket. It has bitten once: four params with long
// help text pushed the config past the old 6144 and the page just lost its connection.
// Sized to the guard, not the other way around: this is permanent BSS and every static
// kilobyte comes straight out of the heap pool (RAM audit 2026-07-31), so keep it snug —
// but when the compile-time worst-case guard in config_params.cpp fires on a new param,
// prefer growing this over butchering help texts (they are the kart's manual) — the guard
// recomputes the exact worst-case bound at every build, so the number never rots here.
constexpr size_t PB_REPLY_CAP = 9216;

// Arming and haptic feedback (named — no magic numbers in the controller).
constexpr float ARM_CENTER_MAX   = 0.08f;    // stick considered "centered" to arm
constexpr float PUSH_MIN         = 0.5f;     // stick considered "pushed" (rumble if blocked)
constexpr int64_t RUMBLE_BLOCK_INTERVAL_US = 800000;   // repetition of the "blocked" rumble
// No sensor of any kind on the wheels (2026-09-29): no encoders, so no speed limiter, no
// PID braking and no rollover protection. The kart is stick → PWM, bounded by duty_cap only.

// Gamepad heartbeat: gamepads stream their HID reports continuously (~10-20 ms).
// Link "connected" but silent past this = communication lost → disarm + braking (the
// Bluetooth supervision timeout, by contrast, takes several seconds). 750 ms, raised from
// 250 ms after real-world driving: Wi-Fi/BT share the one radio, and coexistence can starve
// the HID stream for a few hundred ms — long enough to false-trip the old bound mid-run.
// At ~2.5 m/s top speed (18T→32T chain), 750 ms is ~1.9 m of travel before the brake, versus several
// seconds if we waited for the BT supervision timeout.
constexpr int64_t PAD_HB_TIMEOUT_US  = 750000;
} // namespace hw

// ───────────────────────── Configuration (named fields, persisted) ─────────────────────────
// Named, persisted settings. Each field is a float, or an int32 for integer/bool params
// (accessed through the typed CfgVal/CfgField union — see cfgGet/cfgSet in config_params.cpp).
struct KartConfig
{
    float   duty_cap_frac;  // PWM ceiling (0..1) — the ONLY speed limit
    float   thr_deadzone;   // stick deadzone (forward AND turn)
    float   turn_gain;      // share of the differential at full X stick (0..1)
    int32_t mix_type;       // stick→motor mixing: 0 linear, 1 expo (mixer.hpp)
    float   mix_expo_fwd;   // expo strength on the throttle axis (0 = linear, 1 = cubic)
    float   mix_expo_turn;  // expo strength on the steering axis
    int32_t accel_pct_s;    // how fast the PWM may RISE, % of full duty per second (0 = off)
    int32_t decel_pct_s;    // how fast it may FALL back to 0 — this IS the pseudo-freewheel
    int32_t mot_inv_l;      // 1 = flip the LEFT motor output (convention: +PWM = forward)
    int32_t mot_inv_r;      // 1 = flip the RIGHT motor output
    int32_t mot_swap_lr;    // 1 = LEFT wheel on driver channel R and vice versa (crossed wiring)
    int32_t arm_hold_ms;
    int32_t disarm_s;
    int32_t led_count;
    int32_t led_brightness;

    void setDefaults();
    void clampAll();
};

enum class PType : uint8_t { Float, Int, Bool };

// A 4-byte config scalar: a float, OR an int32 (bools are stored as 0/1 int32). Tagged by
// PType — only ever read/write the member matching the type (see cfgGet/cfgSet).
union CfgVal   { float f; int32_t i; };
union CfgField { float KartConfig::* f; int32_t KartConfig::* i; };

struct ParamDesc
{
    const char* name;   // NVS key + JSON key (≤ 15 characters for NVS)
    const char* desc;   // short label for the web page
    const char* cat;    // category (visual grouping in the config page)
    const char* help;   // long description (tooltip on field hover)
    PType       type;
    CfgVal      min, def, max;   // .f for Float, .i for Int/Bool
    CfgField    field;           // member pointer to the target field (member matching `type`)
};

extern const ParamDesc PARAMS[];
extern const int       PARAM_COUNT;

// Typed access to a parameter, widened to / narrowed from float — the config wire
// (protobuf ParamVal/ParamMeta) stays float; only the internal storage is typed.
float cfgGet(const KartConfig& c, const ParamDesc& p);
void  cfgSet(KartConfig& c, const ParamDesc& p, float v);
float cfgMin(const ParamDesc& p);
float cfgMax(const ParamDesc& p);
float cfgDef(const ParamDesc& p);

// ───────────────────────── Telemetry ─────────────────────────
// 3 (Fault) is RETIRED, not reused: there is no fault state any more (see Stop below).
enum class State : int { Lockout = 0, Calibrate = 1, Run = 2 };
// EFFECTIVE output mode (displayed permanently on the web page), live only — never stored:
// None = driving (A held + stick) · Dynamic = phase short-circuit (A released, disarmed,
// blocked) · Coast = PSEUDO-freewheel (A held, stick centered: the PWM slides down at decel_pct_s).
enum class BrakeMode : int { None = 0, Dynamic = 1, Coast = 2 };

// WHY the kart will not drive — ONE cause, not a mask. This REPLACES the fault mask, the
// Fault enum and the Fault state, all removed on 2026-09-30: nothing on the kart can break
// in a way the firmware can see any more (no voltage sensor, no coil sense, no encoders —
// they went one after the other, each taking its fault bits with it). What was left were four
// ordinary gamepad conditions, none of which is a failure: the gamepad is off, or silent, or
// the driver is holding B, or the sticks were never calibrated. Each one simply disarms and
// brakes — the cause is published so the page and the event log can say WHICH, not because
// something is broken.
// The values are PERSISTED (event log): retire one, never reuse it.
// Order = priority: the first condition that holds is the one reported.
enum class Stop : int
{
    None          = 0,   // nothing in the way — the kart drives, or is merely disarmed
    PadLost       = 1,   // no gamepad connected
    PadStale      = 2,   // "connected" but silent past hw::PAD_HB_TIMEOUT_US (heartbeat)
    EStop         = 3,   // gamepad emergency stop (button B)
    NotCalibrated = 4,   // gamepad connected but its sticks were never calibrated
};
