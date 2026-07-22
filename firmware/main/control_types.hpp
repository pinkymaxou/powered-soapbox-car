// control_types.hpp — PURE control types and constants (compilable on the host).
// Extracted from config.hpp so the control logic (controller_core) and the simulator
// (test_host/sim) share the SAME source of truth: KartConfig, state/fault enums
// and hw:: constants — without any ESP-IDF dependency.
#pragma once

#include <cstdint>

// Float → int rounding (integer/bool fields are stored as float).
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
constexpr int   WDT_TIMEOUT_S = 5;   // also set via sdkconfig (reboot if stalled > 5 s)

constexpr int PWM_FREQ_HZ = 18000;
constexpr int PWM_MAX     = 4095;   // 12 bits (the LEDC resolution lives in hardware.cpp)

// Motors nominally 12 V, driver 6–30 V: the duty is capped AUTOMATICALLY at
// MOTOR_V_NOM / measured Vbat (12 V → ~100%, 20 V → ~60%, 24 V → ~50%), see
// ctl::dutyCapVolts. Vbat is smoothed slowly (τ ≈ 1 s at 500 Hz): without this filtering,
// the sag under load would make the cap oscillate (sag → Vbat drops → duty rises).
constexpr float MOTOR_V_NOM        = 12.0f;
constexpr float VBAT_CAP_EMA_ALPHA = 0.002f;

constexpr int ADC_OVERSAMPLE = 8;   // number of ADS1115 reads averaged (smooths residual noise)
// The ADS1115 in continuous mode at 128 SPS only produces a new value every ~8 ms: reading
// the voltage on every 2 ms tick would waste ~4000 I2C transactions/s re-reading the same
// value. We read at 20 Hz — plenty for the LVC (500 ms debounce) and the PWM cap.
constexpr int VBAT_READ_TICKS = 25;   // 500 Hz / 25 = 20 Hz

// External ADS1115 A/D converter (16-bit, I2C) — replaces the ESP32's internal ADC.
// On bus 0 (with the left AS5600). Address set by ADDR; 0x48 = ADDR→GND.
constexpr uint8_t ADS1115_ADDR = 0x48;

// AS5600 angle sensor (I2C, 12-bit absolute = 4096 counts/turn). Kinematics (see
// doc/reducteur.md): gearbox 16T→80T then 30T→80T = 1:13.33; magnet on the GEARBOX OUTPUT,
// then pulleys 25T→32T (1.28:1) up to the wheel → the sensor turns 1.28 times per wheel turn
// ⇒ GEAR_RATIO = 1.28 (total motor→wheel reduction: 13.33 × 1.28 = 17.07).
// 10" wheel = 0.254 m. Native 3.3 V supply → NO level-shift. Speed = derivative of the angle
// (Δcounts × CTRL_HZ) with 0↔4095 wrap; the SIGN of Δ gives the direction. 500 Hz: unambiguous.
constexpr float AS5600_CPR    = 4096.0f;  // counts per turn (12 bits)
constexpr float GEAR_RATIO    = 1.28f;    // sensor turns per wheel turn (gearbox output, pulleys 32/25)
constexpr float WHEEL_DIAM_M  = 0.254f;   // 10" wheel

constexpr int     I2C_FREQ_HZ       = 400000;  // Fast-mode (the sensor supports up to 1 MHz)
constexpr uint8_t AS5600_ADDR       = 0x36;    // fixed I2C address (a single sensor per bus)
constexpr uint8_t AS5600_REG_RAWANG = 0x0C;    // RAW ANGLE 12-bit (bytes 0x0C MSB / 0x0D LSB)

constexpr int   VBAT_SAG_DEBOUNCE_MS = 500;
constexpr int   LVC_POWEROFF_MS      = 30000;  // auto power cutoff (powerOff) after 30 s below the threshold

// ── Battery: 12 V / 24 V detection at startup, hard-coded LVC thresholds ──
// The voltage must stay STABLE (spread ≤ TOL) for 3 s, then classification: a 12 V even
// at full charge stays ≤ ~14.8 V, a 24 V even discharged stays ≥ ~21 V → the 18 V threshold
// decides unambiguously. We NEVER change battery with the system powered on: type frozen until
// restart. As long as unclassified: no LVC (and the auto PWM cap follows Vbat anyway).
// Lead-acid thresholds per type — not web parameters: tied to chemistry, not to tuning.
constexpr int64_t VBAT_DETECT_STABLE_US = 3000000;   // 3 s of stable voltage
constexpr float   VBAT_DETECT_TOL_V     = 0.5f;      // min-max spread tolerated within the window
constexpr float   VBAT_DETECT_24V_MIN   = 18.0f;     // stable average ≥ 18 V → 24 V, otherwise 12 V
constexpr float   VBAT12_WARN_V = 11.5f, VBAT12_CUT_V = 10.5f, VBAT12_RECOVER_V = 12.0f;
constexpr float   VBAT24_WARN_V = 23.0f, VBAT24_CUT_V = 21.0f, VBAT24_RECOVER_V = 24.0f;
// Full charge AT REST (top of the web gauge; the bottom = cutoff threshold). The display
// scale is decided on the firmware side and sent in the status (batt_lo / batt_hi).
constexpr float   VBAT12_FULL_V = 13.0f;
constexpr float   VBAT24_FULL_V = 26.0f;
constexpr float EBRAKE_MIN_MPS       = 0.15f;  // below this, the wheel is considered stopped (PID braking)
constexpr float ENC_STUCK_PWM        = 0.10f;
constexpr int   ENC_STUCK_MS         = 1000;
// Encoder sanity (wiring/mounting) — a sensor that LIES is worse than an absent sensor:
// any fault below triggers a TOTAL STOP (disarm + brake), latched until reboot.
// · REVERSED: firm command in one direction, wheel measured FIRMLY in the other for 400 ms
//   (evaluated only outside braking: PID braking opposes rotation BY DESIGN).
// · ABERRANT: |speed| physically impossible (kart ≈ 3.3 m/s max) for 200 ms.
constexpr float ENC_REV_PWM          = 0.25f;   // "firm" command
constexpr float ENC_REV_MPS         = 0.30f;    // opposed "firm" speed
constexpr int   ENC_REV_MS          = 400;
constexpr float ENC_REV_DECAY_MPS   = 0.15f;    // |v| decreasing by that much = deceleration, not a reversal
constexpr float ENC_MAX_SANE_MPS    = 8.0f;
constexpr int   ENC_MAD_MS          = 200;
// Speed smoothing (exponential moving average): at 500 Hz the Δangle per tick is quantized
// (~0.08 m/s per count with GEAR_RATIO 1.28 / 10" wheel). α ~0.25 → time constant ~4 ticks (8 ms).
constexpr float SPEED_EMA_ALPHA      = 0.25f;
constexpr int   BTN_DEBOUNCE_TICKS   = 3;

// Arming and haptic feedback (named — no magic numbers in the controller).
constexpr float ARM_CENTER_MAX   = 0.08f;    // stick considered "centered" to arm
constexpr float PUSH_MIN         = 0.5f;     // stick considered "pushed" (rumble if blocked)
constexpr int64_t RUMBLE_BLOCK_INTERVAL_US = 800000;   // repetition of the "blocked" rumble
// Gamepad heartbeat: gamepads stream their HID reports continuously (~10-20 ms).
// Link "connected" but silent > 250 ms = communication lost → IMMEDIATE disarm + braking
// (the Bluetooth supervision timeout, by contrast, takes several seconds).
constexpr int64_t PAD_HB_TIMEOUT_US  = 250000;
} // namespace hw

// ───────────────────────── Configuration (named fields, persisted) ─────────────────────────
// Everything is stored as float (integers/bool too) → homogeneous pointer-to-member.
struct KartConfig
{
    float speed_limit_ms;   // VEHICLE speed limit in FORWARD (m/s)
    float rev_speed_ms;     // speed limit in REVERSE (m/s) — separate
    float duty_cap_frac;
    float thr_deadzone;   // stick deadzone (forward AND turn)
    float thr_ramp_per_s; // slope limiter of the FORWARD command (smoothness, Δ/s)
    float vbat_div_ratio;
    float brk_kp;
    float brk_ki;
    float brk_kd;
    float vlim_kp;
    float vlim_ki;
    float vlim_kd;
    float turn_gain;     // share of the differential at full X stick (0..1)
    float turn_limit_en; // 1 = rollover protection active (speed→turn ramp); 0 = disabled (testing)
    float turn_full_ms;  // below this speed (m/s), turn ±100% (pivot allowed) — rollover protection
    float turn_hi;       // turn limit (0..1) reached at speed_limit_ms (linear ramp)
    float turn_rate;     // max turn slope (Δ/s) — smooths abrupt stick moves
    float vlim_enable;   // 1 = PID speed limiter active; 0 = disabled (testing)
    float brk_pid_enable;// 1 = PID braking active when stopped; 0 = dynamic braking only (testing)
    float use_encoders;  // 1 = speed/brake/fault control via AS5600; 0 = ignore the encoders
    float allow_reverse;
    float arm_hold_ms;
    float disarm_s;
    float led_count;
    float led_brightness;

    // Encoder tick conversion — GIVEN TO THE CONTROLLER BY CONFIGURATION (the host can
    // change it: different sensor/reduction/wheel). Outside PARAMS: neither NVS nor web page —
    // it's hardware, not tuning. Defaults (setDefaults): real AS5600 + reduction gear.
    float enc_mps_per_cps;   // (m/s wheel) per (count/s) — i.e.: meters per count
    float enc_rpm_per_cps;   // (rpm wheel) per (count/s)

    void setDefaults();
    void clampAll();
};

enum class PType : uint8_t { Float, Int, Bool };

struct ParamDesc
{
    const char*        name;   // NVS key + JSON key (≤ 15 characters for NVS)
    const char*        desc;   // short label for the web page
    const char*        cat;    // category (visual grouping in the config page)
    const char*        help;   // long description (tooltip on field hover)
    PType              type;
    float              min, def, max;
    float KartConfig::* field; // pointer to the corresponding field
};

extern const ParamDesc PARAMS[];
extern const int       PARAM_COUNT;

// ───────────────────────── Telemetry ─────────────────────────
enum class State : int { Lockout = 0, Calibrate = 1, Run = 2, Fault = 3 };
// EFFECTIVE braking mode (displayed permanently on the web page):
// Dynamic = phase short-circuit (default state, disarmed, or fallback without encoders);
// Active  = PID braking (speed command 0) — requires encoders present AND brk_pid_enable=1.
enum class BrakeMode : int { None = 0, Dynamic = 1, Active = 2 };
enum class Fault : int { None = 0, EStop = 1, Lvc = 2, NotCalibrated = 3, Encoder = 4, EncoderDir = 5, EncoderMad = 6, EncoderAbsent = 7 };

// Bits of the m_faults mask: ALL conditions active simultaneously (m_fault keeps only
// the highest-priority one). Single source on the firmware side; presentation mirror on the
// web side: FAULTS_DESC in index.html (same bits, texts only).
namespace fb
{
constexpr unsigned ESTOP     = 1u << 0;   // gamepad emergency stop (B)
constexpr unsigned LVC       = 1u << 1;   // low battery
constexpr unsigned NOCAL     = 1u << 2;   // gamepad not calibrated
constexpr unsigned ENC_STUCK = 1u << 3;   // wheel stuck (PWM without rotation)
constexpr unsigned PAD_LOST  = 1u << 4;   // gamepad disconnected
constexpr unsigned NO_VBAT   = 1u << 5;   // voltage sensor absent (info)
constexpr unsigned ENC_REV   = 1u << 6;   // encoder/motor wired backwards
constexpr unsigned ENC_MAD   = 1u << 7;   // aberrant speed measurement
constexpr unsigned ENC_L_ABS = 1u << 8;   // left AS5600 absent (I2C silent) — if use_encoders=1
constexpr unsigned ENC_R_ABS = 1u << 9;   // right AS5600 absent — if use_encoders=1
constexpr unsigned PAD_STALE = 1u << 10;  // gamepad "connected" but silent > 250 ms (heartbeat)

// Aggregates: BLOCKING forbids driving (disarm + State::Fault);
// HARD deserves the strong rumble (every blocking fault except the missing calibration).
constexpr unsigned BLOCKING = LVC | NOCAL | ENC_STUCK | ENC_REV | ENC_MAD | ENC_L_ABS | ENC_R_ABS;
constexpr unsigned HARD     = BLOCKING & ~NOCAL;
} // namespace fb

// PRIORITY fault derived from the bitset — the core publishes ONLY the mask; the Fault enum
// serves only for display (protobuf "fault" field) and test asserts. Same priority order
// as the old controller cascade.
inline Fault primaryFault(unsigned faults)
{
    if (faults & fb::LVC)                         return Fault::Lvc;
    if (faults & (fb::ENC_L_ABS | fb::ENC_R_ABS)) return Fault::EncoderAbsent;
    if (faults & fb::ENC_MAD)                     return Fault::EncoderMad;
    if (faults & fb::ENC_REV)                     return Fault::EncoderDir;
    if (faults & fb::ENC_STUCK)                   return Fault::Encoder;
    if (faults & fb::NOCAL)                       return Fault::NotCalibrated;
    return Fault::None;
}
