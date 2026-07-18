// controller.hpp — Liaison MATÉRIELLE du cœur de contrôle. EspController remplit les deux
// callbacks du KartController (capteurs ← board::, sorties moteur → board::) et lui pousse
// les entrées (manette, bouton START, config web) ; TOUTE la logique métier est dans le
// cœur. Les décisions d'hôte (rumble, coupure d'alimentation, persistance différée) sont
// dérivées de la télémétrie via advisors.hpp. Le namespace Controller n'est que l'AMORÇAGE :
// il initialise l'instance, crée la tâche FreeRTOS 500 Hz et y roule tickOnce().
#pragma once

#include "advisors.hpp"
#include "controller_core.hpp"
#include "input.hpp"

class EspController
{
public:
    void init();       // matériel (board/input) + câblage des callbacks du cœur
    void tickOnce();   // un pas complet : entrées poussées → tick() → décisions d'hôte → publication

private:
    SensorReadings readSensors();                 // callback capteurs : encodeurs + Vbat (en VOLTS batterie)
    void           applyOutputs(const CtrlOutputs& out);   // callback sorties : commande moteur
    void           pushPad();                     // pousse l'état manette au cœur (setPad)
    void           publish(const CtrlTelemetry& t);   // télémétrie + affichage manette → statusPublish

    KartController m_ctrl;       // la logique PURE (identique en simulation)
    input::State   m_in;         // dernier instantané manette complet (champs d'affichage)
    PadInputs      m_pad_in;     // dernier état manette poussé au cœur (pour les conseillers)
    RumbleAdvisor  m_rumble;     // retours haptiques (décision d'hôte)
    PowerOffAdvisor m_poweroff;  // coupure d'alimentation sur LVC prolongée (décision d'hôte)
    bool           m_was_armed = false;   // front armé→désarmé → configFlushPending
};

// Amorçage côté ESP : possède l'instance d'EspController, crée la tâche de contrôle 500 Hz
// (watchdog 5 s) qui appelle tickOnce() en boucle. Rien d'autre.
namespace Controller
{
void init();    // à appeler après configInit
void start();
} // namespace Controller
