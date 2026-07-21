// config_params.cpp — Table des paramètres + défauts/bornes. PUR (compilé aussi sur
// l'hôte pour la simulation) : la source unique de vérité des réglages, sans NVS ni ESP.
#include "control_types.hpp"

#include <algorithm>

// Source unique de vérité : nom (clé NVS/JSON), libellé, catégorie (regroupement visuel),
// aide (infobulle au survol — SANS guillemets doubles, injectée telle quelle dans le JSON),
// type, min/défaut/max, champ visé. L'ordre du tableau = l'ordre d'affichage : garder les
// entrées d'une même catégorie CONSÉCUTIVES (la page web groupe les cat identiques qui se suivent).
// NB : les seuils LVC ne sont PAS ici — batterie 12 V ou 24 V détectée au démarrage,
// seuils codés en dur par type (hw::VBAT12_*/VBAT24_*).
const ParamDesc PARAMS[] =
{
    {"speed_limit_ms",  "Limite vitesse (m/s)",   "Vitesse et puissance",
     "Vitesse maximale en MARCHE AVANT (m/s). Au-dela, le limiteur PID retient le kart (encodeurs requis). C'est aussi la vitesse ou le virage est le plus bride par l'anti-renversement.",
     PType::Float, 0.3f,  3.3f,   7.f,    &KartConfig::speed_limit_ms},
    {"rev_speed_ms",    "Limite recul (m/s)",     "Vitesse et puissance",
     "Vitesse maximale en MARCHE ARRIERE (m/s) — meme limiteur PID, cible choisie selon le sens MESURE. La roulette ne guide pas en recul : rester lent.",
     PType::Float, 0.3f,  1.0f,   3.f,    &KartConfig::rev_speed_ms},
    // Plafond PWM MANUEL (le plus restrictif gagne) — le plafond AUTOMATIQUE 12 V/Vbat
    // mesurée (ctl::dutyCapVolts) s'applique EN PLUS. Défaut 1,0 = laisser faire l'auto.
    {"duty_cap",        "Plafond PWM manuel (0-1)", "Vitesse et puissance",
     "Plafond PWM fixe, en plus du plafond automatique 12 V / tension batterie mesuree (le plus restrictif gagne). Laisser 1,0 en usage normal ; a baisser a la main si on roule sans capteur de tension sur plus de 12 V.",
     PType::Float, 0.05f, 1.0f,   1.f,    &KartConfig::duty_cap_frac},
    {"thr_deadzone",    "Zone morte manche",       "Manette",
     "Rayon autour du centre du stick ou la position est ignoree (0-0,3). Compense un stick qui ne revient pas exactement au centre.",
     PType::Float, 0.f,   0.06f,  0.30f,  &KartConfig::thr_deadzone},
    {"thr_ramp_per_s",  "Douceur avance (D/s)",    "Manette",
     "Pente maximale de la consigne d'avance (pleine echelle par seconde). Plus petit = departs et arrets plus doux.",
     PType::Float, 0.2f,  2.f,    20.f,   &KartConfig::thr_ramp_per_s},
    {"turn_gain",       "Gain de virage (0-1)",    "Manette",
     "Part du differentiel gauche/droite a fond de stick. 1 = pivot sur place a pleine puissance.",
     PType::Float, 0.f,   1.f,    1.f,    &KartConfig::turn_gain},
    {"turn_rate",       "Douceur virage (D/s)",    "Manette",
     "Pente maximale de la consigne de virage (pleine echelle par seconde). Adoucit les coups de stick brusques.",
     PType::Float, 0.3f,  3.0f,   20.f,   &KartConfig::turn_rate},
    // Anti-renversement « iso-a_lat » : virage ±100 % sous turn_full_ms (et partout où
    // 1/v le permet), puis limite ∝ 1/v jusqu'à turn_hi à speed_limit_ms (vitesse MESURÉE),
    // et continue de se resserrer au-delà (emballement).
    {"turn_limit_en",   "Anti-renversement (0/1)", "Anti-renversement",
     "1 = la limite de virage suit la vitesse mesuree (rampe anti-renversement). 0 = desactive — pour les essais au banc uniquement, virage a 100 % a toute vitesse.",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::turn_limit_en},
    {"turn_full_ms",    "Virage 100% sous (m/s)",  "Anti-renversement",
     "Sous cette vitesse vehicule (m/s), le virage est autorise a 100 % (pivot sur place permis). Au-dela, la limite descend lineairement jusqu'a la limite Vmax.",
     PType::Float, 0.1f,  0.5f,   0.8f,   &KartConfig::turn_full_ms},
    // Défaut 0,2 + courbe 1/v (gain de virage 1,0) : la simulation physique montre qu'un
    // CHARGEMENT DÉCALÉ (enfant seul d'un côté, adulte+enfant) renverse avec l'ancienne
    // rampe linéaire dès gain=1 — l'iso-a_lat à 0,2 redonne des marges saines (≥ +0,8 m/s²).
    {"turn_alat_vmax",  "Virage max a Vmax (0-1)", "Anti-renversement",
     "Limite de virage a la vitesse maximale ; entre les deux la limite suit 1/v (meme acceleration laterale a toute vitesse). 0,2 = seule valeur verifiee sure par simulation pour les chargements decales (enfant seul d'un cote, adulte+enfant) avec gain de virage 1.",
     PType::Float, 0.1f,  0.2f,   0.4f,   &KartConfig::turn_hi},
    {"vbat_div_ratio",  "Ratio diviseur Vbat",     "Batterie",
     "Rapport du pont diviseur de mesure de tension : Vbat = tension lue par l'ADS1115 x ce ratio. A calibrer au multimetre. Le type de batterie (12 ou 24 V) et les seuils de coupure sont detectes automatiquement au demarrage.",
     PType::Float, 1.f,   7.667f, 20.f,   &KartConfig::vbat_div_ratio},
    // PID en m/s (l'erreur est en m/s depuis le passage km/h→m/s).
    {"brk_kp",          "Frein PID Kp (m/s)",      "Asservissement (PID)",
     "Gain proportionnel du frein electrique actif (consigne vitesse 0). Encodeurs requis.",
     PType::Float, 0.f,   0.43f,  5.f,    &KartConfig::brk_kp},
    {"brk_ki",          "Frein PID Ki (m/s)",      "Asservissement (PID)",
     "Gain integral du frein electrique actif : rattrape une pente qui fait glisser le kart a l'arret.",
     PType::Float, 0.f,   0.29f,  10.f,   &KartConfig::brk_ki},
    {"brk_kd",          "Frein PID Kd (m/s)",      "Asservissement (PID)",
     "Gain derive du frein electrique actif : amortit les oscillations de freinage.",
     PType::Float, 0.f,   0.011f, 2.f,    &KartConfig::brk_kd},
    {"vlim_enable",     "Limiteur vitesse (0/1)",  "Asservissement (PID)",
     "1 = le PID limiteur retient le kart a la limite de vitesse (encodeurs requis). 0 = desactive — seul le plafond PWM borne la vitesse.",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::vlim_enable},
    {"vlim_kp",         "Limiteur PID Kp (m/s)",   "Asservissement (PID)",
     "Gain proportionnel du limiteur de vitesse (retient le kart a la limite de vitesse).",
     PType::Float, 0.f,   0.54f,  5.f,    &KartConfig::vlim_kp},
    {"vlim_ki",         "Limiteur PID Ki (m/s)",   "Asservissement (PID)",
     "Gain integral du limiteur de vitesse : tient la limite en descente.",
     PType::Float, 0.f,   0.50f,  10.f,   &KartConfig::vlim_ki},
    {"vlim_kd",         "Limiteur PID Kd (m/s)",   "Asservissement (PID)",
     "Gain derive du limiteur de vitesse : amortit les oscillations autour de la limite.",
     PType::Float, 0.f,   0.f,    2.f,    &KartConfig::vlim_kd},
    {"brk_pid_enable",  "Frein PID (0/1)",         "Asservissement (PID)",
     "1 = frein electrique actif (PID, consigne vitesse 0) quand le stick est relache (encodeurs requis). 0 = freinage dynamique seul (court-circuit moteur).",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::brk_pid_enable},
    {"use_encoders",    "Utiliser encodeurs (0/1)", "Comportement",
     "1 = les capteurs AS5600 servent au limiteur de vitesse, au frein PID, a l'anti-renversement et au defaut capteur bloque. 0 = banc d'essai sans encodeurs cables.",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::use_encoders},
    {"allow_reverse",   "Marche arriere (0/1)",    "Comportement",
     "Autoriser la marche arriere (tenue par sa propre limite de vitesse, rev_speed_ms).",
     PType::Bool,  0.f,   1.f,    1.f,    &KartConfig::allow_reverse},
    {"arm_hold_ms",     "Appui armement (ms)",     "Comportement",
     "Duree d'appui maintenu sur START (physique ou manette) pour armer, stick centre exige.",
     PType::Int,   200.f, 1000.f, 5000.f, &KartConfig::arm_hold_ms},
    {"disarm_s",        "Desarmement auto (s)",    "Comportement",
     "Desarmement automatique apres ce delai sans toucher au stick.",
     PType::Int,   5.f,   30.f,   600.f,  &KartConfig::disarm_s},
    {"led_count",       "Nb LEDs ruban",           "LEDs",
     "Nombre de LEDs WS2812 sur le ruban d'etat.",
     PType::Int,   1.f,   10.f,   60.f,   &KartConfig::led_count},
    {"led_brightness",  "Luminosite LEDs",         "LEDs",
     "Luminosite du ruban (1-255).",
     PType::Int,   1.f,   64.f,   255.f,  &KartConfig::led_brightness},
};
const int PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

void KartConfig::setDefaults()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        this->*(PARAMS[i].field) = PARAMS[i].def;
    }
    // Ratios encodeur (hors PARAMS — voir control_types.hpp) : roue 10" via AS5600 + réducteur.
    enc_mps_per_cps = 3.14159265f * hw::WHEEL_DIAM_M / (hw::AS5600_CPR * hw::GEAR_RATIO);
    enc_rpm_per_cps = 60.f / (hw::AS5600_CPR * hw::GEAR_RATIO);
}

void KartConfig::clampAll()
{
    for (int i = 0; i < PARAM_COUNT; ++i)
    {
        float& f = this->*(PARAMS[i].field);
        f = std::clamp(f, PARAMS[i].min, PARAMS[i].max);
    }
    // (Les seuils LVC ne sont plus des paramètres : codés en dur selon la batterie
    // 12/24 V détectée au démarrage — hw::VBAT12_*/VBAT24_*, cohérence garantie.)
}
