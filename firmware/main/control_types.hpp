// control_types.hpp — PURE control types and constants (compilable on the host).
// Extracted from config.hpp so the control logic (controller_core) and the simulator
// (test_host/sim) share the SAME source of truth: KartConfig, state/fault enums
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

// AS5600 angle sensor (I2C, 12-bit absolute = 4096 counts/turn). Kinematics (see
// doc/reducteur.md): gearbox 16T→80T then 30T→80T = 1:13.33; magnet on the GEARBOX OUTPUT,
// then a #35 chain 25T→32T (1.28:1) up to the wheel → the sensor turns 1.28 times per wheel turn
// ⇒ GEAR_RATIO = 1.28 (total motor→wheel reduction: 13.33 × 1.28 = 17.07).
// 10" wheel = 0.254 m. Native 3.3 V supply → NO level-shift. Speed = derivative of the angle
// (Δcounts × CTRL_HZ) with 0↔4095 wrap; the SIGN of Δ gives the direction. 500 Hz: unambiguous.
constexpr float AS5600_CPR    = 4096.0f;  // counts per turn (12 bits)
constexpr float GEAR_RATIO    = 1.28f;    // sensor turns per wheel turn (gearbox output, sprockets 32/25)
constexpr float WHEEL_DIAM_M  = 0.254f;   // 10" wheel

constexpr int     I2C_FREQ_HZ       = 400000;  // Fast-mode (the sensor supports up to 1 MHz)
constexpr int     I2C_XFER_TIMEOUT_MS = 2;     // per-read timeout: small so a bad read can't
                                               // stall the loop >½-turn and alias the absolute
                                               // angle. A real transfer is 3 bytes at 400 kHz
                                               // ≈ 0.1 ms, so 2 ms is still 20x margin — and it
                                               // is the CEILING on what one dead sensor can cost
                                               // the 500 Hz tick (budget 2 ms). Measured on the
                                               // bench with a wheel unplugged: 4 ms timeout gave
                                               // ticks of 3.5-6.7 ms.
constexpr uint8_t AS5600_ADDR       = 0x36;    // fixed I2C address (a single sensor per bus)
constexpr uint8_t AS5600_REG_RAWANG = 0x0C;    // RAW ANGLE 12-bit (bytes 0x0C MSB / 0x0D LSB)
constexpr uint8_t AS5600_REG_STATUS = 0x0B;    // magnet detection register
constexpr uint8_t AS5600_MD         = 0x20;    // STATUS bit 5: magnet detected (in field)
constexpr uint8_t AS5600_ML         = 0x10;    // STATUS bit 4: AGC max → magnet too WEAK / too far
constexpr uint8_t AS5600_MH         = 0x08;    // STATUS bit 3: AGC min → magnet too STRONG / too close
constexpr int     MAG_READ_TICKS    = 50;      // poll STATUS at CTRL_HZ/50 ≈ 10 Hz (not every tick)

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

// Arming and haptic feedback (named — no magic numbers in the controller).
constexpr float ARM_CENTER_MAX   = 0.08f;    // stick considered "centered" to arm
constexpr float PUSH_MIN         = 0.5f;     // stick considered "pushed" (rumble if blocked)
constexpr int64_t RUMBLE_BLOCK_INTERVAL_US = 800000;   // repetition of the "blocked" rumble
// Gamepad heartbeat: gamepads stream their HID reports continuously (~10-20 ms).
// Link "connected" but silent past this = communication lost → disarm + braking (the
// Bluetooth supervision timeout, by contrast, takes several seconds). 750 ms, raised from
// 250 ms after real-world driving: Wi-Fi/BT share the one radio, and coexistence can starve
// the HID stream for a few hundred ms — long enough to false-trip the old bound mid-run.
// At 3.3 m/s top speed, 750 ms is still ~2.5 m of travel before the brake, versus several
// seconds if we waited for the BT supervision timeout.
constexpr int64_t PAD_HB_TIMEOUT_US  = 750000;
} // namespace hw

// ───────────────────────── Configuration (named fields, persisted) ─────────────────────────
// Named, persisted settings. Each field is a float, or an int32 for integer/bool params
// (accessed through the typed CfgVal/CfgField union — see cfgGet/cfgSet in config_params.cpp).
struct KartConfig
{
    float speed_limit_ms;   // VEHICLE speed limit in FORWARD (m/s)
    float rev_speed_ms;     // speed limit in REVERSE (m/s) — separate
    float duty_cap_frac;
    float thr_deadzone;   // stick deadzone (forward AND turn)
    float brk_kp;
    float brk_ki;
    float brk_kd;
    float vlim_kp;
    float vlim_ki;
    float vlim_kd;
    float   turn_gain;      // share of the differential at full X stick (0..1)
    int32_t mix_type;       // stick→motor mixing: 0 linear, 1 expo, 2 expo+speed-soft (mixer.hpp)
    float   mix_expo_fwd;   // expo strength on the throttle axis (0 = linear, 1 = cubic)
    float   mix_expo_turn;  // expo strength on the steering axis
    float   mix_soft_hi;    // SOFT mixer: accelerating-throttle authority left at the speed limit (0..1)
    int32_t turn_limit_en;  // 1 = rollover protection active (speed→turn ramp); 0 = disabled (testing)
    float   turn_full_ms;   // below this speed (m/s), turn ±100% (pivot allowed) — rollover protection
    float   turn_hi;        // turn limit (0..1) reached at speed_limit_ms (1/v iso-a_lat curve)
    float   turn_rate;      // max turn slope (Δ/s) — smooths abrupt stick moves
    int32_t vlim_enable;    // 1 = PID speed limiter active; 0 = disabled (testing)
    int32_t brk_pid_enable; // 1 = PID braking active when stopped; 0 = dynamic braking only (testing)
    int32_t dyn_brake_en;   // 1 = short the motors when the stick is released; 0 = FREEWHEEL (coast)
    int32_t open_loop;      // 1 = TEST: mixed stick → motors, no control loops (limiter/rollover/PID/smoothing)
    int32_t use_encoders;   // 1 = speed/brake/fault control via AS5600; 0 = ignore the encoders
    int32_t enc_inv_l;      // 1 = flip the LEFT encoder's sign (convention: +rpm = forward)
    int32_t enc_inv_r;      // 1 = flip the RIGHT encoder's sign
    int32_t enc_rev_chk;    // 1 = runtime reversed-encoder watchdog (ENC_REV); 0 = commissioning-checked
    float   enc_per_wheel;  // encoder-shaft turns per WHEEL turn (mount: gearbox output 1.28, 1:5 shaft 3.41)
    int32_t arm_hold_ms;
    int32_t disarm_s;
    int32_t led_count;
    int32_t led_brightness;

    // Encoder tick conversion — DERIVED (not stored as params): enc_mps_per_cps from
    // enc_per_wheel (see PARAMS) + the fixed AS5600 CPR and wheel diameter; recomputed by
    // setDefaults()/clampAll() on any config change. enc_rpm_per_cps is the raw shaft rpm.
    float enc_mps_per_cps;   // (m/s wheel) per (count/s) — depends on enc_per_wheel (the mount)
    float enc_rpm_per_cps;   // (encoder-SHAFT rpm) per (count/s) — raw, independent of the ratio

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
enum class State : int { Lockout = 0, Calibrate = 1, Run = 2, Fault = 3 };
// EFFECTIVE braking mode (displayed permanently on the web page):
// Dynamic = phase short-circuit (default state, disarmed, or fallback without encoders);
// Active  = PID braking (speed command 0) — requires encoders present AND brk_pid_enable=1.
enum class BrakeMode : int { None = 0, Dynamic = 1, Active = 2 };
// 2 (Lvc) and 9 (MotorPower) are RETIRED, not reused: no voltage sensor and no coil sense
// any more. The remaining values keep their numbers so a stored event log still reads right.
enum class Fault : int { None = 0, EStop = 1, NotCalibrated = 3, Encoder = 4, EncoderDir = 5, EncoderMad = 6, EncoderAbsent = 7, EncoderMagnet = 8 };

// Bits of the m_faults mask: ALL conditions active simultaneously (m_fault keeps only
// the highest-priority one). Single source on the firmware side; presentation mirror on the
// web side: FAULTS_DESC in index.html (same bits, texts only).
namespace fb
{
constexpr unsigned ESTOP     = 1u << 0;   // gamepad emergency stop (B)
// Bit 1 (LVC), bit 5 (NO_VBAT) and bit 13 (NO_MOTOR_PWR) are RETIRED — no battery
// measurement and no relay-coil sense any more. Their positions stay VACANT rather than
// being reused: the event log on flash stores raw masks, and a recycled bit would make the
// records of an older firmware read as a fault that never happened.
constexpr unsigned NOCAL     = 1u << 2;   // gamepad not calibrated
constexpr unsigned ENC_STUCK = 1u << 3;   // wheel stuck (PWM without rotation)
constexpr unsigned PAD_LOST  = 1u << 4;   // gamepad disconnected
constexpr unsigned ENC_REV   = 1u << 6;   // encoder/motor wired backwards
constexpr unsigned ENC_MAD   = 1u << 7;   // aberrant speed measurement
constexpr unsigned ENC_L_ABS = 1u << 8;   // left AS5600 absent (I2C silent) — if use_encoders=1
constexpr unsigned ENC_R_ABS = 1u << 9;   // right AS5600 absent — if use_encoders=1
constexpr unsigned PAD_STALE = 1u << 10;  // gamepad "connected" but silent (heartbeat, PAD_HB_TIMEOUT_US)
constexpr unsigned MAG_L     = 1u << 11;  // left AS5600 magnet out of field (absent/too far/too close)
constexpr unsigned MAG_R     = 1u << 12;  // right AS5600 magnet out of field

// Aggregates: BLOCKING forbids driving (disarm + State::Fault);
// HARD deserves the strong rumble (every blocking fault except the missing calibration).
// The encoder-SENSOR conditions (ENC_L_ABS/ENC_R_ABS absence, MAG_L/MAG_R magnet-out) are
// NOT here: they are reported regardless of use_encoders (so the bench sees the encoder
// status with use_encoders=0) and only block when use_encoders=1 (handled in step()).
// The emergency stop is no longer in this list at all: the mushroom now opens the main
// relay's coil, which drops the ESP itself — there is nothing left to report.
constexpr unsigned BLOCKING = NOCAL | ENC_STUCK | ENC_REV | ENC_MAD;
constexpr unsigned HARD     = BLOCKING & ~NOCAL;
} // namespace fb

// PRIORITY fault derived from the bitset — the core publishes ONLY the mask; the Fault enum
// serves only for display (protobuf "fault" field) and test asserts. Same priority order
// as the old controller cascade.
inline Fault primaryFault(unsigned faults)
{
    if (faults & (fb::ENC_L_ABS | fb::ENC_R_ABS)) return Fault::EncoderAbsent;
    if (faults & (fb::MAG_L | fb::MAG_R))         return Fault::EncoderMagnet;
    if (faults & fb::ENC_MAD)                     return Fault::EncoderMad;
    if (faults & fb::ENC_REV)                     return Fault::EncoderDir;
    if (faults & fb::ENC_STUCK)                   return Fault::Encoder;
    if (faults & fb::NOCAL)                       return Fault::NotCalibrated;
    return Fault::None;
}
