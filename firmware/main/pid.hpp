// pid.hpp — Régulateur PID réutilisable avec anti-windup (header-only).
#pragma once

#include <algorithm>

class Pid
{
public:
    void reset()
    {
        m_integ = 0;
        m_prev = 0;
    }

    // sp = consigne, meas = mesure, dt = pas (s), gains kp/ki/kd, sortie bornée [out_min..out_max].
    float update(float sp, float meas, float dt, float kp, float ki, float kd, float out_min, float out_max)
    {
        float error = sp - meas;
        float deriv = (dt > 0.f) ? (error - m_prev) / dt : 0.f;
        m_prev = error;

        // Intégrale « candidate » (sera validée seulement si on ne s'enfonce pas dans la saturation).
        float integ_candidate = m_integ + error * dt;
        float out_raw = kp * error + ki * integ_candidate + kd * deriv;
        float out = std::clamp(out_raw, out_min, out_max);

        // Anti-windup (intégration conditionnelle) : si la sortie sature ET que l'erreur
        // pousse encore plus loin dans la saturation, on GÈLE l'intégrale.
        bool winding_up = (out_raw > out_max && error > 0.f) || (out_raw < out_min && error < 0.f);
        if (!winding_up)
        {
            m_integ = integ_candidate;
        }

        return out;
    }

private:
    float m_integ = 0;
    float m_prev = 0;
};
