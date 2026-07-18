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
#include <functional>

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
    // LIMITE DE COURANT par moteur : la borne la plus basse du système est le MOTEUR
    // lui-même — 12 V / ~19,6 A (~172 W) d'après ses spécifications — sous la limite du
    // driver (20 A continu / 60 A crête). Couple, force, appel batterie et énergie sont
    // plafonnés partout à kt·i_max : la simulation ne peut JAMAIS dépasser la capacité
    // théorique des moteurs (blocage réel 85 A et plugging ~150 A sont donc écrêtés).
    float i_max_a   = 19.6f;
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

    // Complaisance de roulis (pneus + châssis + enfants qui penchent) : appui VISIBLE en
    // virage AVANT toute levée de roue — jusqu'à ~4° quand a_lat approche la limite.
    float lean_max_rad = 0.07f;
    float lean_tau_s   = 0.15f;
};

// Défaillances d'encodeur simulables (par roue) — pour tester les défauts du contrôleur.
enum class EncMode { Ok, Absent, Reversed, Stuck, Crazy };

// Mode d'entraînement du pas de simulation :
// Drive = PWM signé appliqué · Brake = court-circuit des phases (frein dynamique) ·
// Float = ALIMENTATION COUPÉE (champignon) : MOSFET ouverts, moteurs flottants, AUCUNE
// force électrique — il ne reste que le roulement et la pente (roue libre).
enum class DriveMode { Drive, Brake, Float };

class Vehicle
{
public:
    explicit Vehicle(const VehicleParams& p = {}) : m_p(p), m_vterm(p.batt_v0) {}

    // Sorties du contrôleur pour ce pas (voir DriveMode). En Drive : PWM signé [-1..1]
    // × plafond duty (cap / PWM_MAX), comme le matériel.
    void step(DriveMode mode, float out_l, float out_r, uint32_t cap, float dt)
    {
        if (m_tipped)
        {
            // RENVERSÉ : le kart est sur le flanc — plus de traction, il glisse et s'arrête
            // (frottement de caisse ~0,4 g), le roulis finit sa course vers ~85°.
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

        // Vitesses de roue actuelles (cinématique différentielle ; ω>0 = virage à droite)
        const float half = m_p.track_m / 2.f;
        const float vl = m_v + m_w * half;
        const float vr = m_v - m_w * half;

        // Tension moteur : PWM × tension batterie AUX BORNES (affaissée par le courant total)
        const float v_l = brake ? 0.f : out_l * duty_cap * m_vterm;
        const float v_r = brake ? 0.f : out_r * duty_cap * m_vterm;
        // NB : le court-circuit (brake) = V = 0 aux bornes → le moteur débite dans Ra seul,
        // couple opposé à ω (freinage dynamique réaliste, s'affaiblit avec la vitesse).

        float fl, fr;
        if (DriveMode::Float == mode || m_air)
        {
            fl = fr = 0.f;      // circuits ouverts (ou roues EN L'AIR) : aucune force au sol
            m_il = m_ir = 0.f;
        }
        else
        {
            fl = wheelForce(v_l, vl, m_il);
            fr = wheelForce(v_r, vr, m_ir);
        }

        // Longitudinal : moteurs − roulement − pente (rien de tout ça en vol)
        const float f_roll = (!m_air && std::fabs(m_v) > 0.02f) ? m_p.roll_n * (m_v > 0 ? 1.f : -1.f) : 0.f;
        const float f_slope = m_air ? 0.f : m_p.mass() * G_MPS2 * std::sin(m_p.slope_rad);
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
        m_power_w = (DriveMode::Drive == mode) ? m_vterm * itot : 0.f;
        m_energy_wh += m_power_w * dt / 3600.f;

        // ── VERTICAL : suivi du sol, décollage balistique sur les arêtes, atterrissage ──
        const float zg = ground_fn ? ground_fn(m_x, m_y) : 0.f;
        if (!m_z_init)
        {
            m_z = m_zg_prev = zg;   // départ POSÉ sur le sol (pas de vz parasite au 1er pas)
            m_z_init = true;
        }
        const float z_ball = m_z + m_vz * dt;              // trajectoire balistique candidate
        const float vz_ball = m_vz - G_MPS2 * dt;
        if (z_ball <= zg + 1e-4f)
        {
            m_air = false;
            // Le sol impose la vitesse verticale — bornée par la géométrie : un sol ne peut
            // pas monter plus vite que la distance parcourue (pente ≤ ~45°). Ça neutralise
            // les discontinuités (entrer dans le FLANC du tremplin = choc, pas une fusée).
            const float vz_max = std::fabs(m_v) + 0.5f;
            float vz_g = (zg - m_zg_prev) / dt;
            if (vz_g >  vz_max) vz_g =  vz_max;
            if (vz_g < -vz_max) vz_g = -vz_max;
            m_vz = vz_g;
            m_z = zg;
        }
        else
        {
            m_air = true;                                  // le sol se dérobe : EN VOL
            m_z = z_ball;
            m_vz = vz_ball;
        }
        m_zg_prev = zg;

        // Tangage : suit la pente au sol ; en vol, s'aligne doucement sur la trajectoire.
        const float pitch_tgt = m_air ? std::atan2(m_vz, std::fabs(m_v) + 0.5f) : m_p.slope_rad;
        m_pitch += (pitch_tgt - m_pitch) * std::fmin(1.f, dt / 0.25f);

        // Complaisance de roulis : appui proportionnel à a_lat/a_tip (visible AVANT la levée).
        const float lean_tgt = m_p.lean_max_rad *
            std::fmax(-1.f, std::fmin(1.f, aLat() / aTip()));
        m_lean += (lean_tgt - m_lean) * std::fmin(1.f, dt / m_p.lean_tau_s);

        // ── ROULIS PHYSIQUE : rotation autour de l'arête chargée du triangle ──
        // Levée quand le moment de l'accélération latérale dépasse le moment de rappel de la
        // gravité ; point de NON-RETOUR quand le CG franchit la verticale de l'arête ; sinon
        // le kart RETOMBE sur ses roues dès que la force relâche. (Simplification : tant que
        // la roue est levée, la dynamique plane v/ω continue inchangée.)
        if (!m_air) stepRoll(dt);   // pas de levée d'arête en vol (déjà en l'air !)

        // Encodeurs : accumulation d'angle CAPTEUR (tours roue × GEAR_RATIO × CPR)
        accumulate(m_acc_l, vlNow());
        accumulate(m_acc_r, vrNow());
    }

    // Intègre le roulis autour de l'arête (φ > 0 = penche à GAUCHE — virage à droite).
    void stepRoll(float dt)
    {
        const float al = aLat();
        if (0 == m_lift)
        {
            // Au sol : une roue lève dès que |a_lat| dépasse la limite du côté chargé.
            if      (al >  aTipLeft())  m_lift = +1;
            else if (-al > aTipRight()) m_lift = -1;
            else return;
        }
        const float s = static_cast<float>(m_lift);
        const float w_e = (m_lift > 0) ? (wEff() - m_p.ycg_m) : (wEff() + m_p.ycg_m);
        const float h = m_p.hcg_m;
        const float phi = s * m_roll;                       // angle positif côté levé
        const float cs = std::cos(phi), sn = std::sin(phi);
        // Rotation autour de l'arête : I·φ̈ = m·a_lat·(h·cosφ + w·sinφ) − m·g·(w·cosφ − h·sinφ)
        // (par unité de masse ; inertie ≈ 1,3·m·(w²+h²) — corps + parallèle-axe approximés)
        const float inertia = 1.3f * (w_e * w_e + h * h);
        const float acc = (s * al * (h * cs + w_e * sn) - G_MPS2 * (w_e * cs - h * sn)) / inertia;
        m_rollrate += acc * dt;
        float nphi = phi + m_rollrate * dt;                 // rate exprimé côté levé (positif)
        if (nphi <= 0.f)
        {
            m_roll = 0.f;                                   // retombe sur ses roues
            m_rollrate = 0.f;
            m_lift = 0;
            return;
        }
        if (nphi >= std::atan2(w_e, h))
        {
            m_tipped = true;                                // CG passé l'arête : non-retour
        }
        m_roll = s * nphi;
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
    float roll() const { return m_roll + m_lean; }   // roulis affiché : levée RIGIDE + appui (complaisance)
    float pitch() const { return m_pitch; }           // tangage (pente au sol / trajectoire en vol)
    float z() const { return m_z; }                   // altitude du châssis
    bool  airborne() const { return m_air; }          // toutes roues EN L'AIR
    int   liftSide() const { return m_lift; }         // 0 au sol, +1 roue droite levée (penche à gauche), -1 inverse
    bool  tipped() const { return m_tipped; }         // renversé (point de non-retour franchi)
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
    // Hauteur du sol sous (x, y) — nullptr = plat (scénarios). Le mode conduite branche le
    // terrain vallonné : le kart le SUIT au sol et DÉCOLLE (balistique) sur les arêtes.
    std::function<float(float, float)> ground_fn;
    EncMode enc_mode_l = EncMode::Ok;
    EncMode enc_mode_r = EncMode::Ok;
    bool    vbat_sensor = true;
    VehicleParams& params() { return m_p; }

private:
    // Force à la roue d'un moteur CC alimenté sous v_applied, roue à v_wheel (m/s).
    // Le courant est ÉCRÊTÉ à ±i_max_a (limite du driver) : couple, force, appel batterie
    // et énergie ne peuvent jamais dépasser la capacité réelle du système.
    float wheelForce(float v_applied, float v_wheel, float& i_out) const
    {
        const float w_motor = (v_wheel / m_p.wheel_r_m) * m_p.gear;   // rad/s côté moteur
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
    float m_v = 0.f, m_w = 0.f;          // état dynamique
    float m_x = 0.f, m_y = 0.f, m_h = 0.f, m_t = 0.f;
    float m_vterm;
    float m_il = 0.f, m_ir = 0.f;        // courants moteurs (pour le sag batterie)
    float m_z = 0.f, m_vz = 0.f;         // altitude / vitesse verticale (saut !)
    bool  m_z_init = false;              // premier pas : se poser sur le sol réel
    float m_zg_prev = 0.f;               // hauteur du sol au pas précédent
    bool  m_air = false;                 // toutes roues en l'air (balistique)
    float m_pitch = 0.f;                 // tangage affiché
    float m_lean = 0.f;                  // complaisance de roulis (appui en virage)
    float m_roll = 0.f;                  // roulis autour de l'arête (rad, signé)
    float m_rollrate = 0.f;              // vitesse de roulis (rad/s, magnitude côté levé)
    int   m_lift = 0;                    // arête chargée : +1 gauche levée… voir liftSide()
    bool  m_tipped = false;
    float m_power_w = 0.f;               // puissance batterie instantanée (estimation)
    float m_energy_wh = 0.f;             // énergie batterie cumulée (estimation)
    double m_acc_l = 0.0, m_acc_r = 0.0; // angle capteur accumulé (counts, fractionnaire)
    long   m_last_l = 0, m_last_r = 0;
};

} // namespace sim
