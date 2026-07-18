// vehicle.hpp — Modèle physique du kart (pur, header-only) pour la simulation hôte.
// Dynamique différentielle plane (corps rigide : vitesse longitudinale + lacet), moteurs CC
// avec force contre-électromotrice, batterie avec résistance interne, capteurs simulés
// (AS5600 quantifiés, tension), pente optionnelle, et CRITÈRE DE RENVERSEMENT du tricycle.
//
// Fidélité « ordre de grandeur » : les paramètres estimés (Ra, Iz, h_cg, x_cg, frottements)
// sont regroupés dans VehicleParams, commentés, et recalables avec des mesures réelles.
// Les constantes PARTAGÉES avec le firmware (roue, réducteur, CPR) viennent de hw:: —
// source unique de vérité (control_types.hpp).
#pragma once

#include <cmath>
#include <cstdint>

#include "control_types.hpp"

namespace sim
{

constexpr float G_MPS2 = 9.81f;
constexpr float PI_F   = 3.14159265f;

struct VehicleParams
{
    // ── Masses / géométrie (concept doc/cad/kart_concept.scad + README §1) ──
    float mass_kart_kg = 32.f;  // châssis bois + moteurs + batterie + électronique
    float mass_pass_kg = 66.f;  // PASSAGERS : 2 enfants ~10 ans (≈ 33 kg chacun)
    float mass() const { return mass_kart_kg + mass_pass_kg; }
    float track_m   = 0.84f;    // voie avant (centres des roues motrices)
    float wb_m      = 0.765f;   // empattement essieu → pivot roulette
    float iz_kgm2   = 12.f;     // inertie de lacet (estimée : m·(L/2)²·k)
    float xcg_m     = 0.40f;    // CG derrière l'essieu avant (batterie au museau)
    float hcg_m     = 0.38f;    // hauteur du CG chargé (enfants assis bas, qui bougent un peu)
    float ycg_m     = 0.f;      // décalage LATÉRAL du CG (+ = gauche) — chargement asymétrique

    // ── Moteur CC 12 V (par roue) — 4615 tr/min à vide, ~19,6 A nominal ──
    float ke        = 12.f / (4615.f * 2.f * PI_F / 60.f);   // V·s/rad ≈ 0,0248 (= kt)
    float ra_ohm    = 0.15f;    // résistance d'induit (blocage ≈ 85 A, ~4× le nominal 19,6 A)
    float gear      = 16.f;     // réduction totale moteur → roue (1:16,0)
    float eta       = 0.85f;    // rendement boîte + courroie
    float wheel_r_m = hw::WHEEL_DIAM_M / 2.f;

    // ── Résistances à l'avancement ──
    float roll_n    = 30.f;     // frottement de roulement (N, opposé au mouvement)
    float yaw_damp  = 4.5f;    // amortissement de lacet N·m·s/rad — faible : la roulette folle SUIT

    // ── Batterie (plomb 12 V par défaut ; V0=25,6/rint doublé pour du 24 V) ──
    float batt_v0   = 12.8f;    // tension à vide (pleine charge au repos)
    float batt_rint = 0.05f;    // résistance interne (Ω) — l'affaissement sous charge
    float vdiv      = 7.667f;   // pont diviseur de la mesure (≈ hw:: défaut vbat_div_ratio)

    float slope_rad = 0.f;      // pente (+ = montée) — F = m·g·sin(θ) opposée à l'avance
};

// Défaillances d'encodeur simulables (par roue) — pour tester les défauts du contrôleur.
enum class EncMode { Ok, Absent, Reversed, Stuck, Crazy };

class Vehicle
{
public:
    explicit Vehicle(const VehicleParams& p = {}) : m_p(p), m_vterm(p.batt_v0) {}

    // Sorties du contrôleur pour ce pas. brake=true → court-circuit des phases (freinage
    // dynamique) ; sinon PWM signé [-1..1] × plafond duty (cap / PWM_MAX) comme le matériel.
    void step(bool brake, float out_l, float out_r, uint32_t cap, float dt)
    {
        const float duty_cap = static_cast<float>(cap) / hw::PWM_MAX;

        // Vitesses de roue actuelles (cinématique différentielle ; ω>0 = virage à droite)
        const float half = m_p.track_m / 2.f;
        const float vl = m_v + m_w * half;
        const float vr = m_v - m_w * half;

        // Tension moteur : PWM × tension batterie AUX BORNES (affaissée par le courant total)
        const float v_l = brake ? 0.f : out_l * duty_cap * m_vterm;
        const float v_r = brake ? 0.f : out_r * duty_cap * m_vterm;
        // NB : le court-circuit (brake) = V = 0 aux bornes → le moteur débite dans Ra seul,
        // couple opposé à ω (freinage dynamique réaliste, s'affaiblit avec la vitesse).

        const float fl = wheelForce(v_l, vl, m_il);
        const float fr = wheelForce(v_r, vr, m_ir);

        // Longitudinal : moteurs − roulement − pente
        const float f_roll = (std::fabs(m_v) > 0.02f) ? m_p.roll_n * (m_v > 0 ? 1.f : -1.f) : 0.f;
        const float f_slope = m_p.mass() * G_MPS2 * std::sin(m_p.slope_rad);
        const float dv = (fl + fr - f_roll - f_slope) / m_p.mass();

        // Lacet : différence de force × demi-voie, amorti (roulette, ripage des pneus)
        const float dw = ((fl - fr) * half - m_p.yaw_damp * m_w) / m_p.iz_kgm2;

        m_v += dv * dt;
        m_w += dw * dt;
        if (std::fabs(m_v) < 0.005f && std::fabs(dv) < 0.05f && brake) m_v = 0.f;   // repos

        // Pose (pour la visualisation et les distances d'arrêt)
        m_x += m_v * std::cos(m_h) * dt;
        m_y += m_v * std::sin(m_h) * dt;
        m_h += m_w * dt;
        m_t += dt;

        // Batterie : tension aux bornes selon le courant total tiré
        const float itot = std::fabs(m_il) + std::fabs(m_ir);
        m_vterm = m_p.batt_v0 - m_p.batt_rint * itot;
        if (m_vterm < 0.f) m_vterm = 0.f;

        // ÉNERGIE tirée de la batterie (estimation) : P = V_bornes × Σ|i| pendant la traction
        // et le frein PID (plugging = courant batterie) ; le court-circuit (frein dynamique)
        // dissipe dans le moteur SANS tirer sur la batterie. Pas de récupération modélisée.
        m_power_w = brake ? 0.f : m_vterm * itot;
        m_energy_wh += m_power_w * dt / 3600.f;

        // Encodeurs : accumulation d'angle CAPTEUR (tours roue × GEAR_RATIO × CPR)
        accumulate(m_acc_l, vlNow());
        accumulate(m_acc_r, vrNow());
    }

    // ── Capteurs pour SimController ──
    // Δcounts AS5600 depuis le dernier appel (quantifié comme le vrai 12 bits).
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
        if (EncMode::Crazy == mode) d += 900;   // ~+55 m/s : physiquement impossible
        return static_cast<int>(d);
    }
    bool encPresent(bool left) const
    {
        return (left ? enc_mode_l : enc_mode_r) != EncMode::Absent;
    }
    // Tension à la broche ADC (après pont diviseur), −1 si capteur retiré.
    float vbatPinVolts() const { return vbat_sensor ? (m_vterm / m_p.vdiv) : -1.f; }

    // ── Grandeurs physiques (asserts + affichage) ──
    float v() const { return m_v; }         // m/s (signée)
    float yawRate() const { return m_w; }   // rad/s (+ = droite)
    float aLat() const { return m_v * m_w; }
    float x() const { return m_x; }
    float y() const { return m_y; }
    float heading() const { return m_h; }
    float t() const { return m_t; }
    float vterm() const { return m_vterm; }
    float powerW() const { return m_power_w; }       // puissance batterie instantanée (estimée)
    float energyWh() const { return m_energy_wh; }   // énergie batterie cumulée (estimée)
    float wheelV(bool left) const { return left ? vlNow() : vrNow(); }

    // Accélération latérale de basculement du TRICYCLE : le triangle de sustentation se
    // rétrécit de la voie (essieu) à zéro (roulette) → demi-largeur EFFECTIVE au droit du CG :
    // w_eff = (voie/2)·(1 − x_cg/empattement), ± le décalage latéral y_cg. Virage à DROITE
    // (a_lat > 0) → la force centrifuge pousse à GAUCHE → bascule sur l'arête GAUCHE, dont
    // la marge est réduite si le chargement est déjà à gauche (y_cg > 0) — et inversement.
    float aTipLeft()  const { return G_MPS2 * (wEff() - m_p.ycg_m) / m_p.hcg_m; }
    float aTipRight() const { return G_MPS2 * (wEff() + m_p.ycg_m) / m_p.hcg_m; }
    float aTip() const { return std::fmin(aTipLeft(), aTipRight()); }   // pire côté (affichage)
    // Marge de renversement (m/s²) : > 0 = stable, ≤ 0 = le kart lève une roue —
    // calculée contre l'arête que la manœuvre COURANTE charge réellement.
    float tipMargin() const
    {
        const float limit = (aLat() >= 0.f) ? aTipLeft() : aTipRight();
        return limit - std::fabs(aLat());
    }

    // ── Réglages de scénario ──
    EncMode enc_mode_l = EncMode::Ok;
    EncMode enc_mode_r = EncMode::Ok;
    bool    vbat_sensor = true;
    VehicleParams& params() { return m_p; }

private:
    // Force à la roue d'un moteur CC alimenté sous v_applied, roue à v_wheel (m/s).
    float wheelForce(float v_applied, float v_wheel, float& i_out) const
    {
        const float w_motor = (v_wheel / m_p.wheel_r_m) * m_p.gear;   // rad/s côté moteur
        const float i = (v_applied - m_p.ke * w_motor) / m_p.ra_ohm;
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
    float m_v = 0.f, m_w = 0.f;          // état dynamique
    float m_x = 0.f, m_y = 0.f, m_h = 0.f, m_t = 0.f;
    float m_vterm;
    float m_il = 0.f, m_ir = 0.f;        // courants moteurs (pour le sag batterie)
    float m_power_w = 0.f;               // puissance batterie instantanée (estimation)
    float m_energy_wh = 0.f;             // énergie batterie cumulée (estimation)
    double m_acc_l = 0.0, m_acc_r = 0.0; // angle capteur accumulé (counts, fractionnaire)
    long   m_last_l = 0, m_last_r = 0;
};

} // namespace sim
