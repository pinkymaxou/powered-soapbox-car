// advisors.hpp — Décisions d'HÔTE dérivées de la télémétrie du cœur (KartController).
// VOLONTAIREMENT hors du cœur : le contrôleur de base calcule une sortie MOTEUR en
// fonction des entrées, rien d'autre. Vibrer la manette (présentation) et couper
// l'alimentation (politique système) appartiennent à l'hôte — ces deux conseillers PURS
// (compilables hôte) sont partagés par l'ESP (EspController) et la simulation (SimController).
#pragma once

#include <cmath>
#include <cstdint>

#include "controller_core.hpp"

// Commande de vibration à transmettre à la manette (input::rumble côté ESP).
struct RumbleCmd
{
    bool     active = false;
    uint8_t  strong = 0;
    uint8_t  weak = 0;
    uint16_t duration_ms = 0;
};

// Retours haptiques sur les FRONTS de la télémétrie : armement (doux), défaut dur ou
// e-stop (fort), « pousse le manche mais bloqué » (fort, répété avec anti-spam).
class RumbleAdvisor
{
public:
    RumbleCmd update(const CtrlTelemetry& t, const PadInputs& pad, int64_t now_us)
    {
        RumbleCmd cmd;
        auto set = [&cmd](uint8_t st, uint8_t wk, uint16_t ms) {
            cmd.active = true;
            cmd.strong = st;
            cmd.weak = wk;
            cmd.duration_ms = ms;
        };
        // Défaut « dur » : LVC ou n'importe quel défaut encodeur — tous forcent l'arrêt
        // et méritent un retour haptique appuyé.
        const bool hard_fault = (0 != (t.faults & fb::HARD));
        if (t.armed && !m_armed_prev)                                        // vient d'être armé → doux
            set(90, 160, 220);
        if ((hard_fault && !m_hard_prev) || (pad.estop && !m_estop_prev))    // erreur soudaine / e-stop → fort
            set(255, 255, 450);
        const bool pushing = (std::fabs(pad.rx) > hw::PUSH_MIN) || (std::fabs(pad.ry) > hw::PUSH_MIN);
        const bool can_drive = (State::Run == t.state);   // Run ⟺ armé et aucune condition bloquante
        if (pushing && !can_drive && (now_us - m_block_us) > hw::RUMBLE_BLOCK_INTERVAL_US)
        {
            set(220, 220, 250);   // bouge le manche mais bloqué → fort (répété)
            m_block_us = now_us;
        }
        m_armed_prev = t.armed;
        m_estop_prev = pad.estop;
        m_hard_prev = hard_fault;
        return cmd;
    }

private:
    bool    m_armed_prev = false;
    bool    m_estop_prev = false;
    bool    m_hard_prev = false;
    int64_t m_block_us = 0;
};

// Coupure d'alimentation : LVC active sans interruption pendant hw::LVC_POWEROFF_MS →
// true (l'hôte coupe : board::powerOff côté ESP). Le cœur signale fb::LVC, c'est tout.
class PowerOffAdvisor
{
public:
    bool update(const CtrlTelemetry& t, int64_t now_us)
    {
        if (0 == (t.faults & fb::LVC))
        {
            m_since_us = 0;
            return false;
        }
        if (0 == m_since_us) m_since_us = now_us;
        return (now_us - m_since_us) > static_cast<int64_t>(hw::LVC_POWEROFF_MS) * 1000;
    }

private:
    int64_t m_since_us = 0;   // début de la période LVC ininterrompue
};
