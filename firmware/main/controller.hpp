// controller.hpp — Contrôle des moteurs (boucle temps réel) du kart électrique.
#pragma once

void kartInit();    // initialise le matériel (à appeler après configInit)
void kartStart();   // démarre la tâche de contrôle 100 Hz (watchdog 5 s)
