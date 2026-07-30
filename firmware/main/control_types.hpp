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
constexpr int   WDT_TIMEOUT_S = 5;   // also set via sdkconfig (reboot if stalled > 5 s)

constexpr int PWM_FREQ_HZ = 18000;
constexpr int PWM_MAX     = 4095;   // 12 bits (the LEDC resolution lives in hardware.cpp)

// Motors nominally 12 V, driver 6–30 V: the duty is capped AUTOMATICALLY at
// MOTOR_V_NOM / measured Vbat (12 V → ~100%, 20 V → ~60%, 24 V → ~50%), see
// ctl::dutyCapVolts. Vbat is smoothed slowly (τ ≈ 1 s at 500 Hz): without this filtering,
// the sag under load would make the cap oscillate (sag → Vbat drops → duty rises).
constexpr float MOTOR_V_NOM        = 12.0f;
constexpr float VBAT_CAP_EMA_ALPHA = 0.002f;

// LVC input smoothing. The 10.5 V cutoff is a lead-acid figure AT REST, and it was being
// applied to a reading taken UNDER LOAD: ~20 A through ~0.05 Ω sags the pack about 2 V, so a
// perfectly healthy half-charged battery dips to ~10.0 V for the ~0.6 s an acceleration lasts
// — long enough to clear the 500 ms debounce and cut the kart dead mid-manoeuvre. Measured in
// simulation: at 12.0 V open-circuit the old code cut 0.55 s after the throttle opened.
// Feeding the LVC a slow EMA fixes it WITHOUT weakening the protection, because a genuinely
// flat pack sits below the threshold at rest too and is still caught at the same instant.
// Sweep over the sag probe: τ of 1–3 s classifies every test pack correctly, τ ≥ 5 s starts
// missing a worn one. 2 s is the middle of that band.
constexpr float VBAT_LVC_EMA_TAU_S = 2.0f;
constexpr float VBAT_LVC_EMA_ALPHA = CTRL_DT_S / VBAT_LVC_EMA_TAU_S;   // ≈ 0.001 at 500 Hz

constexpr int ADC_OVERSAMPLE = 8;   // number of ADS1115 reads averaged (smooths residual noise)
// The ADS1115 in continuous mode at 128 SPS only produces a new value every ~8 ms: reading
// the voltage on every 2 ms tick would waste ~4000 I2C transactions/s re-reading the same
// value. We read at 20 Hz — plenty for the LVC (500 ms debounce) and the PWM cap.
constexpr int VBAT_READ_TICKS = 25;   // 500 Hz / 25 = 20 Hz

// External ADS1115 A/D converter (16-bit, I2C) — replaces the ESP32's internal ADC.
// On bus 0 (with the left AS5600). Address set by ADDR; 0x48 = ADDR→GND.
constexpr uint8_t ADS1115_ADDR = 0x48;

// Battery measurement divider bridge on A0. Fixed by the resistors soldered on the board, so
// it is a CONSTANT and not a settable parameter — a wrong value silently misreports the
// battery and drags the LVC thresholds along with it. Swap the resistors ⇒ edit the two
// values below and reflash; the ratio follows on its own.
//
//   Vbat + ──[ R_TOP ]──┬──[ R_BOTTOM ]── GND (switched by the latch)
//                       └── A0        ⇒  V_adc = Vbat × R_BOTTOM / (R_TOP + R_BOTTOM)
//
// ⚠️ Sizing rule: V_adc must stay under the 3.3 V rail at the pack's MAXIMUM voltage (on
// charge), otherwise the ADS1115 is driven past its absolute maximum. 100 k/15 k suits a 12 V
// pack (1.93 V at 14.8 V); a 24 V pack needs 100 k/12 k.
constexpr float VBAT_R_TOP    = 100000.0f;   // R1, from Vbat+ to the A0 node (Ω)
constexpr float VBAT_R_BOTTOM =  15000.0f;   // R2, from the A0 node to ground (Ω)
constexpr float VBAT_DIV_RATIO = (VBAT_R_TOP + VBAT_R_BOTTOM) / VBAT_R_BOTTOM;   // ≈ 7.667

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
constexpr size_t PB_REPLY_CAP = 10240;

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
    int32_t turn_limit_en;  // 1 = rollover protection active (speed→turn ramp); 0 = disabled (testing)
    float   turn_full_ms;   // below this speed (m/s), turn ±100% (pivot allowed) — rollover protection
    float   turn_hi;        // turn limit (0..1) reached at speed_limit_ms (1/v iso-a_lat curve)
    float   turn_rate;      // max turn slope (Δ/s) — smooths abrupt stick moves
    int32_t vlim_enable;    // 1 = PID speed limiter active; 0 = disabled (testing)
    int32_t brk_pid_enable; // 1 = PID braking active when stopped; 0 = dynamic braking only (testing)
    int32_t dyn_brake_en;   // 1 = short the motors when the stick is released; 0 = FREEWHEEL (coast)
    int32_t open_loop;      // 1 = TEST: mixed stick → motors, no control loops (limiter/rollover/PID/smoothing)
    int32_t use_encoders;   // 1 = speed/brake/fault control via AS5600; 0 = ignore the encoders
    int32_t vbat_check_en;  // 1 = the LVC can block driving and cut power; 0 = voltage shown only
    int32_t enc_inv_l;      // 1 = flip the LEFT encoder's sign (convention: +rpm = forward)
    int32_t enc_inv_r;      // 1 = flip the RIGHT encoder's sign
    int32_t pwr_sense_en;   // 1 = the motor-power opto sense is wired and blocks when it reads dead
    int32_t idle_off_min;   // minutes disarmed before self power-off (0 = never)
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
enum class Fault : int { None = 0, EStop = 1, Lvc = 2, NotCalibrated = 3, Encoder = 4, EncoderDir = 5, EncoderMad = 6, EncoderAbsent = 7, EncoderMagnet = 8, MotorPower = 9 };

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
constexpr unsigned PAD_STALE = 1u << 10;  // gamepad "connected" but silent (heartbeat, PAD_HB_TIMEOUT_US)
constexpr unsigned MAG_L     = 1u << 11;  // left AS5600 magnet out of field (absent/too far/too close)
constexpr unsigned MAG_R     = 1u << 12;  // right AS5600 magnet out of field
// MOTOR power rail dead while the logic rail is alive — which, in the two-rail wiring, is
// exactly what pressing the emergency stop looks like from the ESP's point of view. Reported
// only when pwr_sense_en=1 (the opto is wired); blocking is handled in step(), like the
// encoder-sensor bits, so a bench without the opto is unaffected.
constexpr unsigned NO_MOTOR_PWR = 1u << 13;

// Aggregates: BLOCKING forbids driving (disarm + State::Fault);
// HARD deserves the strong rumble (every blocking fault except the missing calibration).
// The encoder-SENSOR conditions (ENC_L_ABS/ENC_R_ABS absence, MAG_L/MAG_R magnet-out) are
// NOT here: they are reported regardless of use_encoders (so the bench sees the encoder
// status with use_encoders=0) and only block when use_encoders=1 (handled in step()).
constexpr unsigned BLOCKING = LVC | NOCAL | ENC_STUCK | ENC_REV | ENC_MAD;
constexpr unsigned HARD     = BLOCKING & ~NOCAL;
} // namespace fb

// PRIORITY fault derived from the bitset — the core publishes ONLY the mask; the Fault enum
// serves only for display (protobuf "fault" field) and test asserts. Same priority order
// as the old controller cascade.
inline Fault primaryFault(unsigned faults)
{
    // Motor rail dead outranks everything: nothing else can be acted on until it is back.
    if (faults & fb::NO_MOTOR_PWR)                return Fault::MotorPower;
    if (faults & fb::LVC)                         return Fault::Lvc;
    if (faults & (fb::ENC_L_ABS | fb::ENC_R_ABS)) return Fault::EncoderAbsent;
    if (faults & (fb::MAG_L | fb::MAG_R))         return Fault::EncoderMagnet;
    if (faults & fb::ENC_MAD)                     return Fault::EncoderMad;
    if (faults & fb::ENC_REV)                     return Fault::EncoderDir;
    if (faults & fb::ENC_STUCK)                   return Fault::Encoder;
    if (faults & fb::NOCAL)                       return Fault::NotCalibrated;
    return Fault::None;
}
