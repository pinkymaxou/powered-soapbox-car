// sim_controller.hpp — Hôte SIMULÉ du contrôleur : même câblage qu'EspController, mais les
// callbacks du KartController lisent le modèle physique (vehicle.hpp) et lui appliquent la
// commande moteur ; la manette est SCRIPTÉE (fonction du temps). Les décisions d'hôte
// (rumble, coupure) passent par les MÊMES conseillers que l'ESP (advisors.hpp). Horloge
// virtuelle : un tick = hw::CTRL_DT_S exactement. C'est le « mockup » des tests et du
// visualisateur.
#pragma once

#include <cstdint>
#include <functional>

#include "advisors.hpp"
#include "controller_core.hpp"
#include "vehicle.hpp"

namespace sim
{

// Commande de la manette scriptée à l'instant t (secondes de simulation).
struct PadCmd
{
    float x = 0.f;            // virage [-1..1] (déjà « calibré » : le script parle en consigne)
    float y = 0.f;            // avance [-1..1]
    bool  start = false;      // bouton START/Options tenu
    bool  estop = false;      // bouton B
    bool  connected = true;
    bool  reports = true;     // false = lien « connecté » mais plus AUCUN rapport (heartbeat)
    bool  sys_power = true;   // false = CHAMPIGNON : alimentation coupée (ESP32 éteint,
                              // MOSFET ouverts → roue libre) — le contrôleur ne tourne plus
};
using PadScript = std::function<PadCmd(float t)>;

// Maintien de START pendant [t0, t0+dur] — l'armement standard des scénarios.
inline bool held(float t, float t0, float dur) { return t >= t0 && t <= t0 + dur; }

class SimController
{
public:
    SimController(Vehicle& veh, PadScript script)
        : m_veh(veh), m_script(std::move(script))
    {
        m_ctrl.setCallbacks([this] { return readSensors(); },
                            [this](const CtrlOutputs& out) { applyOutputs(out); });
    }

    // Un pas complet de simulation : manette → contrôleur → décisions d'hôte → physique.
    void stepOnce(const KartConfig& cfg)
    {
        m_now_us += static_cast<int64_t>(hw::CTRL_DT_S * 1e6f);
        m_cmd = m_script(m_veh.t());
        if (!m_cmd.sys_power)
        {
            // Alimentation coupée : le contrôleur NE TOURNE PLUS (pas de tick), les ponts
            // sont ouverts — le véhicule continue sa vie en roue libre.
            m_veh.step(DriveMode::Float, 0.f, 0.f, 0, hw::CTRL_DT_S);
            return;
        }
        if (m_cmd.connected && m_cmd.reports) m_last_report_us = m_now_us;

        PadInputs pad;
        pad.x = m_cmd.x;
        pad.y = m_cmd.y;
        pad.rx = m_cmd.x;   // le script parle en consigne : brut = calibré
        pad.ry = m_cmd.y;
        pad.connected = m_cmd.connected;
        pad.calibrated = calibrated;
        pad.estop = m_cmd.estop;
        pad.start = m_cmd.start;
        pad.last_report_us = m_last_report_us;

        m_vdiv = cfg.vbat_div_ratio;   // conversion volts broche → volts batterie (readSensors)
        m_ctrl.setPad(pad);
        m_ctrl.setConfig(cfg);         // les scénarios/le mode conduite changent cfg au vol
        m_ctrl.tick(m_now_us);

        // Décisions d'HÔTE — mêmes conseillers que l'ESP : rumble compté, coupure mémorisée.
        const CtrlTelemetry t = m_ctrl.telemetry();
        if (m_rumble.update(t, pad, m_now_us).active) ++rumbles;
        powered_off |= m_poweroff.update(t, m_now_us);

        m_veh.step(m_brake_out ? DriveMode::Brake : DriveMode::Drive,
                   m_out_l, m_out_r, m_cap, hw::CTRL_DT_S);
    }

    CtrlTelemetry telemetry() const { return m_ctrl.telemetry(); }

    bool powered() const { return m_cmd.sys_power; }

    bool calibrated = true;    // la calibration est un préalable, pas l'objet de la physique
    int  rumbles = 0;          // nb de vibrations émises (RumbleAdvisor)
    bool powered_off = false;  // coupure demandée (PowerOffAdvisor : LVC prolongée)

    // Entrée manette BRUTE du script (avant deadzone/rampes/anti-renversement)
    float padX() const { return m_cmd.x; }
    float padY() const { return m_cmd.y; }

    // Dernière commande moteur envoyée au « matériel » (inspectable par les tests)
    float lastOutL() const { return m_out_l; }
    float lastOutR() const { return m_out_r; }
    bool  lastBrake() const { return m_brake_out; }

private:
    // Callback capteurs — miroir d'EspController::readSensors, le modèle physique à la
    // place du bus I2C : les pannes (absent/inversé/figé/fou) viennent des EncMode du véhicule.
    SensorReadings readSensors()
    {
        SensorReadings s;
        s.enc_delta_l = m_veh.encDelta(true);
        s.enc_delta_r = m_veh.encDelta(false);
        s.enc_ok_l = m_veh.encPresent(true);
        s.enc_ok_r = m_veh.encPresent(false);
        const float pin_v = m_veh.vbatPinVolts();
        s.vbat_ok = (pin_v >= 0.f);
        s.vbat_v = s.vbat_ok ? pin_v * m_vdiv : -1.f;
        return s;
    }

    // Callback sorties : mémorise la commande moteur (appliquée au véhicule par stepOnce).
    void applyOutputs(const CtrlOutputs& out)
    {
        m_brake_out = out.dyn_brake;
        m_out_l = out.dyn_brake ? 0.f : out.out_l;
        m_out_r = out.dyn_brake ? 0.f : out.out_r;
        if (!out.dyn_brake) m_cap = out.cap;
    }

    KartController  m_ctrl;
    Vehicle&        m_veh;
    PadScript       m_script;
    PadCmd          m_cmd;
    RumbleAdvisor   m_rumble;
    PowerOffAdvisor m_poweroff;
    int64_t   m_now_us = 0;
    int64_t   m_last_report_us = 0;
    float     m_vdiv = 1.f;
    float     m_out_l = 0.f, m_out_r = 0.f;
    uint32_t  m_cap = hw::PWM_MAX;
    bool      m_brake_out = true;
};

} // namespace sim
