// vehicle.hpp — Physics model of the kart (pure, header-only) for the host simulation.
// Planar differential dynamics (rigid body: longitudinal speed + yaw), DC motors
// with back-EMF, battery with internal resistance, simulated sensors
// (quantized AS5600, voltage), optional slope, and the tricycle's ROLLOVER CRITERION.
//
// "Order of magnitude" fidelity: the estimated parameters (Ra, Iz, h_cg, x_cg, frictions)
// are gathered in VehicleParams, commented, and recalibratable with real measurements.
// The constants SHARED with the firmware (wheel, reduction gear, CPR) come from hw:: —
// single source of truth (control_types.hpp).
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>

#include "control_types.hpp"

namespace sim
{

constexpr float G_MPS2 = 9.81f;
constexpr float PI_F   = 3.14159265f;

struct VehicleParams
{
    // ── Masses / geometry (concept doc/cad/kart_concept.scad + README §1) ──
    float mass_kart_kg = 32.f;  // wood chassis + motors + battery + electronics
    float mass_pass_kg = 66.f;  // PASSENGERS: 2 children ~10 y.o. (≈ 33 kg each)
    float mass() const { return mass_kart_kg + mass_pass_kg; }
    float track_m   = 0.84f;    // front track width (centers of the drive wheels)
    float wb_m      = 0.765f;   // wheelbase axle → caster wheel pivot
    float iz_kgm2   = 12.f;     // yaw inertia (estimated: m·(L/2)²·k)
    float xcg_m     = 0.40f;    // CG behind the front axle (battery at the nose)
    float hcg_m     = 0.38f;    // height of the loaded CG (children sitting low, moving a bit)
    float ycg_m     = 0.f;      // LATERAL offset of the CG (+ = left) — asymmetric load

    // ── 12 V DC motor (per wheel) — 4615 rpm no-load, ~19.6 A nominal ──
    float ke        = 12.f / (4615.f * 2.f * PI_F / 60.f);   // V·s/rad ≈ 0.0248 (= kt)
    float ra_ohm    = 0.15f;    // armature resistance (stall ≈ 85 A, ~4× the 19.6 A nominal)
    // CURRENT LIMIT per motor: the lowest bound in the system is the MOTOR
    // itself — 12 V / ~19.6 A (~172 W) per its specifications — below the driver
    // limit (20 A continuous / 60 A peak). Torque, force, battery draw and energy are
    // capped everywhere at kt·i_max: the simulation can NEVER exceed the theoretical
    // capacity of the motors (real stall 85 A and plugging ~150 A are therefore clamped).
    float i_max_a   = 19.6f;
    float gear      = 17.07f;   // total motor → wheel reduction (1:17.07)
    float eta       = 0.85f;    // gearbox + belt efficiency
    float wheel_r_m = hw::WHEEL_DIAM_M / 2.f;

    // ── Resistances to motion ──
    float roll_n    = 30.f;     // rolling resistance (N, opposing the motion)
    float yaw_damp  = 4.5f;    // yaw damping N·m·s/rad — low: the free caster wheel FOLLOWS

    // ── Battery (12 V lead-acid by default; V0=25.6/rint doubled for 24 V) ──
    float batt_v0   = 12.8f;    // open-circuit voltage (full charge at rest)
    float batt_rint = 0.05f;    // internal resistance (Ω) — the sag under load
    float vdiv      = 7.667f;   // measurement voltage divider (≈ hw:: default vbat_div_ratio)

    float slope_rad = 0.f;      // slope (+ = uphill) — F = m·g·sin(θ) opposing forward motion

    // Roll compliance (tires + chassis + children leaning): VISIBLE lean in a
    // turn BEFORE any wheel lift — up to ~4° when a_lat approaches the limit.
    float lean_max_rad = 0.07f;
    float lean_tau_s   = 0.15f;
};

// Simulatable encoder failures (per wheel) — to test the controller's faults.
enum class EncMode { Ok, Absent, Reversed, Stuck, Crazy };

// Drive mode for the simulation step:
// Drive = signed PWM applied · Brake = phase short-circuit (dynamic braking) ·
// Float = POWER CUTOFF (kill switch): MOSFETs open, motors floating, NO
// electric force — only rolling resistance and slope remain (coasting).
enum class DriveMode { Drive, Brake, Float };

class Vehicle
{
public:
    explicit Vehicle(const VehicleParams& p = {}) : m_p(p), m_vterm(p.batt_v0) {}

    // Controller outputs for this step (see DriveMode). In Drive: signed PWM [-1..1]
    // × duty cap (cap / PWM_MAX), like the hardware.
    void step(DriveMode mode, float out_l, float out_r, uint32_t cap, float dt)
    {
        if (m_tipped)
        {
            // TIPPED: the kart is on its side — no more traction, it slides and stops
            // (body friction ~0.4 g), the roll finishes its travel toward ~85°.
            const float dec = 0.4f * G_MPS2 * dt;
            if (std::fabs(m_v) > dec) m_v -= (m_v > 0 ? dec : -dec); else m_v = 0.f;
            m_w = 0.f;
            m_roll += (m_roll > 0 ? 1.f : -1.f) * 2.5f * dt;
            if (std::fabs(m_roll) > 1.48f) m_roll = (m_roll > 0 ? 1.48f : -1.48f);
            m_x += m_v * std::cos(m_h) * dt;
            m_y += m_v * std::sin(m_h) * dt;
            m_t += dt;
            m_power_w = 0.f;
            m_il = m_ir = 0.f;
            return;
        }
        const bool brake = (DriveMode::Brake == mode);
        const float duty_cap = static_cast<float>(cap) / hw::PWM_MAX;

        // Current wheel speeds (differential kinematics; ω>0 = turning right)
        const float half = m_p.track_m / 2.f;
        const float vl = m_v + m_w * half;
        const float vr = m_v - m_w * half;

        // Motor voltage: PWM × battery TERMINAL voltage (sagged by the total current)
        const float v_l = brake ? 0.f : out_l * duty_cap * m_vterm;
        const float v_r = brake ? 0.f : out_r * duty_cap * m_vterm;
        // NB: the short-circuit (brake) = V = 0 at the terminals → the motor discharges into Ra alone,
        // torque opposing ω (realistic dynamic braking, weakens with speed).

        float fl, fr;
        if (DriveMode::Float == mode || m_air)
        {
            fl = fr = 0.f;      // open circuits (or wheels AIRBORNE): no force on the ground
            m_il = m_ir = 0.f;
        }
        else
        {
            fl = wheelForce(v_l, vl, m_il);
            fr = wheelForce(v_r, vr, m_ir);
        }

        // Longitudinal: motors − rolling resistance − slope (none of that while airborne)
        const float f_roll = (!m_air && std::fabs(m_v) > 0.02f) ? m_p.roll_n * (m_v > 0 ? 1.f : -1.f) : 0.f;
        const float f_slope = m_air ? 0.f : m_p.mass() * G_MPS2 * std::sin(m_p.slope_rad);
        const float dv = (fl + fr - f_roll - f_slope) / m_p.mass();

        // Yaw: force difference × half-track, damped (caster wheel, tire scrub)
        const float dw = ((fl - fr) * half - m_p.yaw_damp * m_w) / m_p.iz_kgm2;

        m_v += dv * dt;
        m_w += dw * dt;
        if (std::fabs(m_v) < 0.005f && std::fabs(dv) < 0.05f && brake) m_v = 0.f;   // at rest

        // Pose (for visualization and stopping distances)
        m_x += m_v * std::cos(m_h) * dt;
        m_y += m_v * std::sin(m_h) * dt;
        m_h += m_w * dt;
        m_t += dt;

        // Battery: terminal voltage as a function of the total current drawn
        const float itot = std::fabs(m_il) + std::fabs(m_ir);
        m_vterm = m_p.batt_v0 - m_p.batt_rint * itot;
        if (m_vterm < 0.f) m_vterm = 0.f;

        // ENERGY drawn from the battery (estimate): P = V_term × Σ|i| during traction
        // and active (PID) braking (plugging = battery current); the short-circuit (dynamic braking)
        // dissipates in the motor WITHOUT drawing on the battery. No regeneration modeled.
        m_power_w = (DriveMode::Drive == mode) ? m_vterm * itot : 0.f;
        m_energy_wh += m_power_w * dt / 3600.f;

        // ── VERTICAL: ground tracking, ballistic take-off on edges, landing ──
        const float zg = ground_fn ? ground_fn(m_x, m_y) : 0.f;
        if (!m_z_init)
        {
            m_z = m_zg_prev = zg;   // start RESTING on the ground (no spurious vz on the 1st step)
            m_z_init = true;
        }
        const float z_ball = m_z + m_vz * dt;              // candidate ballistic trajectory
        const float vz_ball = m_vz - G_MPS2 * dt;
        if (z_ball <= zg + 1e-4f)
        {
            m_air = false;
            // The ground imposes the vertical speed — bounded by geometry: the ground cannot
            // rise faster than the distance traveled (slope ≤ ~45°). This neutralizes
            // discontinuities (hitting the SIDE of the ramp = a jolt, not a rocket).
            const float vz_max = std::fabs(m_v) + 0.5f;
            float vz_g = (zg - m_zg_prev) / dt;
            if (vz_g >  vz_max) vz_g =  vz_max;
            if (vz_g < -vz_max) vz_g = -vz_max;
            m_vz = vz_g;
            m_z = zg;
        }
        else
        {
            m_air = true;                                  // the ground drops away: AIRBORNE
            m_z = z_ball;
            m_vz = vz_ball;
        }
        m_zg_prev = zg;

        // Pitch: follows the ground slope; while airborne, gently aligns with the trajectory.
        const float pitch_tgt = m_air ? std::atan2(m_vz, std::fabs(m_v) + 0.5f) : m_p.slope_rad;
        m_pitch += (pitch_tgt - m_pitch) * std::fmin(1.f, dt / 0.25f);

        // Roll compliance: lean proportional to a_lat/a_tip (visible BEFORE the lift).
        const float lean_tgt = m_p.lean_max_rad *
            std::fmax(-1.f, std::fmin(1.f, aLat() / aTip()));
        m_lean += (lean_tgt - m_lean) * std::fmin(1.f, dt / m_p.lean_tau_s);

        // ── PHYSICAL ROLL: rotation around the loaded edge of the triangle ──
        // Lift when the moment of the lateral acceleration exceeds the restoring moment of
        // gravity; POINT OF NO RETURN when the CG crosses the vertical of the edge; otherwise
        // the kart FALLS BACK onto its wheels as soon as the force releases. (Simplification: as long as
        // the wheel is lifted, the planar v/ω dynamics continue unchanged.)
        if (!m_air) stepRoll(dt);   // no edge lift while airborne (already in the air!)

        // Encoders: SENSOR angle accumulation (wheel turns × GEAR_RATIO × CPR)
        accumulate(m_acc_l, vlNow());
        accumulate(m_acc_r, vrNow());
    }

    // Integrates the roll around the edge (φ > 0 = leaning LEFT — turning right).
    void stepRoll(float dt)
    {
        const float al = aLat();
        if (0 == m_lift)
        {
            // On the ground: a wheel lifts as soon as |a_lat| exceeds the limit of the loaded side.
            if      (al >  aTipLeft())  m_lift = +1;
            else if (-al > aTipRight()) m_lift = -1;
            else return;
        }
        const float s = static_cast<float>(m_lift);
        const float w_e = (m_lift > 0) ? (wEff() - m_p.ycg_m) : (wEff() + m_p.ycg_m);
        const float h = m_p.hcg_m;
        const float phi = s * m_roll;                       // positive angle on the lifted side
        const float cs = std::cos(phi), sn = std::sin(phi);
        // Rotation around the edge: I·φ̈ = m·a_lat·(h·cosφ + w·sinφ) − m·g·(w·cosφ − h·sinφ)
        // (per unit mass; inertia ≈ 1.3·m·(w²+h²) — body + parallel-axis approximated)
        const float inertia = 1.3f * (w_e * w_e + h * h);
        const float acc = (s * al * (h * cs + w_e * sn) - G_MPS2 * (w_e * cs - h * sn)) / inertia;
        m_rollrate += acc * dt;
        float nphi = phi + m_rollrate * dt;                 // rate expressed on the lifted side (positive)
        if (nphi <= 0.f)
        {
            m_roll = 0.f;                                   // falls back onto its wheels
            m_rollrate = 0.f;
            m_lift = 0;
            return;
        }
        if (nphi >= std::atan2(w_e, h))
        {
            m_tipped = true;                                // CG past the edge: no return
        }
        m_roll = s * nphi;
    }

    // ── Sensors for SimController ──
    // AS5600 Δcounts since the last call (quantized like the real 12-bit).
    int encDelta(bool left)
    {
        EncMode mode = left ? enc_mode_l : enc_mode_r;
        double& acc  = left ? m_acc_l : m_acc_r;
        long&   last = left ? m_last_l : m_last_r;
        if (EncMode::Absent == mode || EncMode::Stuck == mode) return 0;
        const long cur = static_cast<long>(std::floor(acc));
        long d = cur - last;
        last = cur;
        if (EncMode::Reversed == mode) d = -d;
        if (EncMode::Crazy == mode) d += 900;   // ~+55 m/s: physically impossible
        return static_cast<int>(d);
    }
    bool encPresent(bool left) const
    {
        return (left ? enc_mode_l : enc_mode_r) != EncMode::Absent;
    }
    // Voltage at the ADC pin (after voltage divider), −1 if the sensor is removed.
    float vbatPinVolts() const { return vbat_sensor ? (m_vterm / m_p.vdiv) : -1.f; }

    // ── Physical quantities (asserts + display) ──
    float v() const { return m_v; }         // m/s (signed)
    float yawRate() const { return m_w; }   // rad/s (+ = right)
    float aLat() const { return m_v * m_w; }
    float x() const { return m_x; }
    float y() const { return m_y; }
    float heading() const { return m_h; }
    float t() const { return m_t; }
    float vterm() const { return m_vterm; }
    float roll() const { return m_roll + m_lean; }   // displayed roll: RIGID lift + lean (compliance)
    float pitch() const { return m_pitch; }           // pitch (ground slope / airborne trajectory)
    float z() const { return m_z; }                   // chassis altitude
    bool  airborne() const { return m_air; }          // all wheels AIRBORNE
    int   liftSide() const { return m_lift; }         // 0 on the ground, +1 right wheel lifted (leaning left), -1 the reverse
    bool  tipped() const { return m_tipped; }         // tipped (point of no return crossed)
    float powerW() const { return m_power_w; }       // instantaneous battery power (estimated)
    float energyWh() const { return m_energy_wh; }   // cumulative battery energy (estimated)
    float wheelV(bool left) const { return left ? vlNow() : vrNow(); }

    // Lateral tipping acceleration of the TRICYCLE: the support triangle
    // shrinks from the track width (axle) to zero (caster wheel) → EFFECTIVE half-width at the CG:
    // w_eff = (track/2)·(1 − x_cg/wheelbase), ± the lateral offset y_cg. Turning RIGHT
    // (a_lat > 0) → the centrifugal force pushes LEFT → tips onto the LEFT edge, whose
    // margin is reduced if the load is already on the left (y_cg > 0) — and vice versa.
    float aTipLeft()  const { return G_MPS2 * (wEff() - m_p.ycg_m) / m_p.hcg_m; }
    float aTipRight() const { return G_MPS2 * (wEff() + m_p.ycg_m) / m_p.hcg_m; }
    float aTip() const { return std::fmin(aTipLeft(), aTipRight()); }   // worst side (display)
    // Tip margin (m/s²): > 0 = stable, ≤ 0 = the kart lifts a wheel —
    // computed against the edge that the CURRENT maneuver actually loads.
    float tipMargin() const
    {
        const float limit = (aLat() >= 0.f) ? aTipLeft() : aTipRight();
        return limit - std::fabs(aLat());
    }

    // ── Scenario settings ──
    // Ground height under (x, y) — nullptr = flat (scenarios). Driving mode hooks up the
    // hilly terrain: the kart FOLLOWS it on the ground and TAKES OFF (ballistic) on the edges.
    std::function<float(float, float)> ground_fn;
    EncMode enc_mode_l = EncMode::Ok;
    EncMode enc_mode_r = EncMode::Ok;
    bool    vbat_sensor = true;
    VehicleParams& params() { return m_p; }

private:
    // Wheel force of a DC motor powered at v_applied, wheel at v_wheel (m/s).
    // The current is CLAMPED to ±i_max_a (driver limit): torque, force, battery draw
    // and energy can never exceed the real capacity of the system.
    float wheelForce(float v_applied, float v_wheel, float& i_out) const
    {
        const float w_motor = (v_wheel / m_p.wheel_r_m) * m_p.gear;   // rad/s motor side
        float i = (v_applied - m_p.ke * w_motor) / m_p.ra_ohm;
        if (i >  m_p.i_max_a) i =  m_p.i_max_a;
        if (i < -m_p.i_max_a) i = -m_p.i_max_a;
        i_out = i;
        const float torque_wheel = m_p.ke * i * m_p.gear * m_p.eta;
        return torque_wheel / m_p.wheel_r_m;
    }

    float wEff() const { return (m_p.track_m / 2.f) * (1.f - m_p.xcg_m / m_p.wb_m); }
    float vlNow() const { return m_v + m_w * m_p.track_m / 2.f; }
    float vrNow() const { return m_v - m_w * m_p.track_m / 2.f; }

    void accumulate(double& acc, float v_wheel)
    {
        const double wheel_rps = v_wheel / (PI_F * hw::WHEEL_DIAM_M);
        acc += wheel_rps * hw::GEAR_RATIO * hw::AS5600_CPR * (1.0 / hw::CTRL_HZ);
    }

    VehicleParams m_p;
    float m_v = 0.f, m_w = 0.f;          // dynamic state
    float m_x = 0.f, m_y = 0.f, m_h = 0.f, m_t = 0.f;
    float m_vterm;
    float m_il = 0.f, m_ir = 0.f;        // motor currents (for the battery sag)
    float m_z = 0.f, m_vz = 0.f;         // altitude / vertical speed (jump!)
    bool  m_z_init = false;              // first step: settle on the real ground
    float m_zg_prev = 0.f;               // ground height at the previous step
    bool  m_air = false;                 // all wheels airborne (ballistic)
    float m_pitch = 0.f;                 // displayed pitch
    float m_lean = 0.f;                  // roll compliance (lean in a turn)
    float m_roll = 0.f;                  // roll around the edge (rad, signed)
    float m_rollrate = 0.f;              // roll rate (rad/s, magnitude on the lifted side)
    int   m_lift = 0;                    // loaded edge: +1 left lifted… see liftSide()
    bool  m_tipped = false;
    float m_power_w = 0.f;               // instantaneous battery power (estimate)
    float m_energy_wh = 0.f;             // cumulative battery energy (estimate)
    double m_acc_l = 0.0, m_acc_r = 0.0; // accumulated sensor angle (counts, fractional)
    long   m_last_l = 0, m_last_r = 0;
};

} // namespace sim
