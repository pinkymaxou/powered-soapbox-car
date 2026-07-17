// control_math.hpp — Math de contrôle PURE (header-only, sans dépendance ESP-IDF).
// Extraite de controller.cpp / input.cpp pour être partagée ET testable sur l'hôte
// (voir test_host/). Toute fonction ici doit rester libre d'effets de bord.
#pragma once

#include <cmath>
#include <cstdint>

namespace ctl
{

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Zone morte avec remappage : [dz..1] → [0..1] (continu, symétrique).
inline float deadzone(float v, float dz)
{
    if (std::fabs(v) <= dz) return 0.f;
    const float s = (v > 0.f) ? 1.f : -1.f;
    return s * (std::fabs(v) - dz) / (1.f - dz);
}

// Limiteur de pente : rapproche `current` de `target` d'au plus rate·dt.
inline float slew(float target, float current, float rate, float dt)
{
    const float step = rate * dt;
    const float diff = target - current;
    if (diff > step)  return current + step;
    if (diff < -step) return current - step;
    return target;
}

// Mix arcade différentiel : gauche = avance + virage·gain, droite = avance − virage·gain.
inline void mixArcade(float fwd, float turn, float gain, float& out_l, float& out_r)
{
    // Stick à gauche (turn < 0) → la roue DROITE accélère et la GAUCHE ralentit (et
    // inversement) : le kart tourne du côté du stick.
    out_l = clampf(fwd + turn * gain, -1.f, 1.f);
    out_r = clampf(fwd - turn * gain, -1.f, 1.f);
}

// Anti-renversement « rampe » : limite de virage selon la vitesse VÉHICULE MESURÉE (m/s).
// |v| ≤ v_full → ±100 % (pivot sur place permis, v≈0) ; puis décroissance LINÉAIRE jusqu'à
// hi_limit (ex. 0,5 = ±50 %) atteint à v_max. Ex. v_full=0,5 : sous 0,5 m/s on tourne à fond.
inline float turnLimit(float v_abs, float v_full, float v_max, float hi_limit)
{
    if (v_abs <= v_full) return 1.f;
    const float span = (v_max - v_full > 0.1f) ? (v_max - v_full) : 0.1f;
    const float f = clampf((v_abs - v_full) / span, 0.f, 1.f);
    return 1.f + f * (hi_limit - 1.f);
}

// Plafond PWM automatique selon la tension batterie MESURÉE : les moteurs (v_nom, ex. 12 V)
// ne doivent pas voir plus que leur tension nominale en moyenne → duty max = v_nom / vbat.
// Batterie 12 V → ~100 %, 20 V → ~60 %, 24 V → ~50 %. Tension inconnue (capteur absent,
// vbat ≤ 0) ou plus basse que v_nom → 1 (pas de bridage automatique).
inline float dutyCapVolts(float vbat, float v_nom)
{
    if (vbat <= v_nom) return 1.f;
    return v_nom / vbat;
}

// Détection du TYPE de batterie au démarrage (12 V ou 24 V, plomb) : la tension doit
// rester STABLE (écart min-max ≤ tol) pendant stable_us, puis classement par seuil —
// une 12 V même en pleine charge reste ≤ ~14,8 V, une 24 V même déchargée reste ≥ ~21 V,
// donc v24_min entre les deux tranche sans ambiguïté. On ne change JAMAIS de batterie
// système allumé : le résultat est DÉFINITIF jusqu'au redémarrage (0 = pas encore classée).
struct BattDetect
{
    int64_t m_start_us = -1;   // début de la fenêtre de stabilité (-1 = pas commencée)
    float   m_min = 0.f, m_max = 0.f;
    int     volts = 0;         // 0 = inconnue, puis 12 ou 24 (figé)

    void update(float v, int64_t now_us, int64_t stable_us, float tol, float v24_min)
    {
        if (0 != volts) return;                                  // déjà classée : définitif
        if (v <= 0.f) { m_start_us = -1; return; }               // mesure invalide → on repart
        if (m_start_us < 0 || (v - m_min > tol) || (m_max - v > tol))
        {
            m_start_us = now_us;                                 // instable → fenêtre relancée
            m_min = m_max = v;
            return;
        }
        m_min = (v < m_min) ? v : m_min;
        m_max = (v > m_max) ? v : m_max;
        if (now_us - m_start_us >= stable_us)
        {
            volts = (0.5f * (m_min + m_max) >= v24_min) ? 24 : 12;
        }
    }
};

// Détection « capteur/moteur câblé à l'envers » : commande franche d'un côté, roue mesurée
// FRANCHEMENT de l'autre pendant win_us SANS que sa vitesse ne décroisse. Le point clé :
// une DÉCÉLÉRATION commandée (freinage au stick, kart lancé) a aussi une vitesse opposée à
// la commande — mais elle FOND vers zéro ; un câblage inversé donne une vitesse opposée
// STABLE ou CROISSANTE. On ré-ancre la fenêtre dès que |v| décroît de plus de `decay`.
struct RevDetect
{
    int64_t m_t0 = 0;    // début de la fenêtre d'opposition (0 = inactive)
    float   m_v0 = 0.f;  // |v| à l'ancrage

    // true = inversion CONFIRMÉE (opposition franche, persistante et non décroissante).
    bool update(float out, float v, int64_t now_us, int64_t win_us,
                float out_min, float v_min, float decay)
    {
        const bool opposed = (std::fabs(out) > out_min) && (std::fabs(v) > v_min) &&
                             (out * v < 0.f);
        if (!opposed)
        {
            m_t0 = 0;
            return false;
        }
        if (0 == m_t0 || std::fabs(v) < m_v0 - decay)   // début, ou ça décélère → ré-ancre
        {
            m_t0 = now_us;
            m_v0 = std::fabs(v);
            return false;
        }
        return (now_us - m_t0) > win_us;
    }

    void reset() { m_t0 = 0; }
};

// Compensation cercle→carré : le stick physique est borné par un CERCLE (x²+y²≤1) ; en
// diagonale pleine chaque axe plafonne à ~0,71. Étire radialement (direction constante,
// facteur |v|/max(|x|,|y|), =√2 en diagonale) pour rendre les coins du CARRÉ atteignables.
inline void squareMap(float& x, float& y)
{
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float m = (ax > ay) ? ax : ay;
    if (m > 1e-3f)
    {
        const float scale = std::sqrt(x * x + y * y) / m;
        x = clampf(x * scale, -1.f, 1.f);
        y = clampf(y * scale, -1.f, 1.f);
    }
}

} // namespace ctl
