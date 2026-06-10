// controller.hpp — Contrôle des moteurs (boucle temps réel) du kart électrique.
#pragma once

namespace Controller
{
void init();    // initialise le matériel (à appeler après configInit)
void start();   // démarre la tâche de contrôle 500 Hz (watchdog 5 s)
} // namespace Controller
