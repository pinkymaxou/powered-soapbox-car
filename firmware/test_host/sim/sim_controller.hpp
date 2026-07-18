// sim_controller.hpp — Implémentation SIMULÉE du contrôleur : les callbacks io* de
// ControllerBase sont branchés sur le modèle physique (vehicle.hpp) et sur une manette
// SCRIPTÉE (fonction du temps). Horloge virtuelle : un tick = hw::CTRL_DT_S exactement.
// C'est le « mockup » des tests automatisés et du visualisateur — même logique que l'ESP.
#pragma once

#include <cstdint>
#include <functional>

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

class SimController final : public ControllerBase
{
public:
    SimController(Vehicle& veh, PadScript script)
        : m_veh(veh), m_script(std::move(script)) {}

    // Un pas complet de simulation : manette → contrôleur → physique.
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
        tick(cfg);
        m_veh.step(m_brake_out ? DriveMode::Brake : DriveMode::Drive,
                   m_out_l, m_out_r, m_cap, hw::CTRL_DT_S);
    }

    bool powered() const { return m_cmd.sys_power; }

    bool calibrated = true;    // la calibration est un préalable, pas l'objet de la physique
    int  rumbles = 0;          // nb de vibrations émises (retours haptiques)
    bool powered_off = false;  // ioPowerOff appelé (LVC prolongée)

    // Entrée manette BRUTE du script (avant deadzone/rampes/anti-renversement)
    float padX() const { return m_cmd.x; }
    float padY() const { return m_cmd.y; }

    // Dernières sorties envoyées au « matériel » (inspectables par les tests)
    float lastOutL() const { return m_out_l; }
    float lastOutR() const { return m_out_r; }
    bool  lastBrake() const { return m_brake_out; }

protected:
    int64_t ioNowUs() override { return m_now_us; }

    CtrlPad ioPad() override
    {
        CtrlPad p;
        p.x = m_cmd.x;
        p.y = m_cmd.y;
        p.rx = m_cmd.x;    // le script parle en consigne : brut = calibré
        p.ry = m_cmd.y;
        p.connected = m_cmd.connected;
        p.estop = m_cmd.estop;
        p.start = m_cmd.start;
        return p;
    }

    int64_t ioPadLastReportUs() override { return m_last_report_us; }
    bool    ioPadCalibrated() override   { return calibrated; }
    bool    ioBtnStart() override        { return false; }   // START physique : non utilisé en sim
    float   ioVbatRaw() override         { return m_veh.vbatPinVolts(); }
    int     ioEncDeltaL() override       { return m_veh.encDelta(true); }
    int     ioEncDeltaR() override       { return m_veh.encDelta(false); }
    bool    ioEncPresentL() override     { return m_veh.encPresent(true); }
    bool    ioEncPresentR() override     { return m_veh.encPresent(false); }

    void ioMotorsSet(float l, float r, uint32_t cap) override
    {
        m_brake_out = false;
        m_out_l = l;
        m_out_r = r;
        m_cap = cap;
    }
    void ioMotorsBrake() override
    {
        m_brake_out = true;
        m_out_l = 0.f;
        m_out_r = 0.f;
    }
    void ioRumble(uint8_t, uint8_t, uint16_t) override { ++rumbles; }
    void ioPowerOff() override { powered_off = true; }

private:
    Vehicle&  m_veh;
    PadScript m_script;
    PadCmd    m_cmd;
    int64_t   m_now_us = 0;
    int64_t   m_last_report_us = 0;
    float     m_out_l = 0.f, m_out_r = 0.f;
    uint32_t  m_cap = hw::PWM_MAX;
    bool      m_brake_out = true;
};

} // namespace sim
