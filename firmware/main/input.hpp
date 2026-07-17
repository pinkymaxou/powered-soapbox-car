// input.hpp — Entrée de pilotage (manette). Abstraction : le reste du firmware ne dépend
// pas du backend (Bluepad32 / autre). Axes normalisés [-1..1], + état de connexion.
//
// Convention arcade : y = avance (+1 = avant), x = virage (+1 = vers la droite).
// Le mixage différentiel se fait dans controller.cpp.
#pragma once

#include <cstdint>

namespace input
{
int64_t lastReportUs();   // date (µs, esp_timer) du dernier rapport HID — heartbeat manette

struct State
{
    float x = 0.f;          // virage CALIBRÉ [-1..1]  (+ = droite)
    float y = 0.f;          // avance CALIBRÉ [-1..1]  (+ = avant)
    float rx = 0.f;         // virage BRUT (non calibré) [-1..1] — pour l'affichage
    float ry = 0.f;         // avance BRUT (non calibré) [-1..1] — pour l'affichage
    float zl = 0.f;         // gâchette analogique gauche (ZL) [0..1] — affichage
    float zr = 0.f;         // gâchette analogique droite (ZR) [0..1] — affichage
    float rx2 = 0.f;        // stick DROIT X [-1..1] — affichage seulement (non calibré)
    float ry2 = 0.f;        // stick DROIT Y [-1..1] — affichage seulement (non calibré)
    uint32_t buttons = 0;   // masque : boutons | (misc<<16) | (dpad<<24) — affichage
    bool  connected = false; // manette appairée ET connectée
    bool  estop = false;     // bouton d'arrêt manette (ex. B) — frein immédiat
    bool  start = false;     // bouton START/Options manette — armement (comme le bouton physique)
};

// Retour haptique : fait vibrer la manette (magnitudes 0..255, durée en ms).
// Sûr à appeler depuis n'importe quelle tâche (la requête est relayée au thread BT).
void rumble(uint8_t strong, uint8_t weak, uint16_t dur_ms);

void        init();          // démarre le backend (BT) — appelé une fois au boot
State       get();           // dernier état (axes CALIBRÉS [-1..1] ; {0,0} si non calibré)
void        startPairing();  // ouvre une fenêtre d'appairage. ⚠️ EFFACE la calibration.
void        unpair();        // oublie la manette appairée et déconnecte (efface aussi la calibration)
bool        pairing();       // true si fenêtre d'appairage ouverte
const char* name();          // modèle de la manette ("" si aucune)
int         battery();       // niveau batterie manette 0..100, -1 si inconnu

// Calibration manette — OBLIGATOIRE pour conduire (le contrôleur refuse de rouler sinon).
// Séquence : calStart() manche au repos (capture le centre) → bouger les sticks à fond
// dans tous les sens (capture des extrêmes) → calFinish() (calcule l'échelle, persiste en NVS).
bool calibrated();   // true si une calibration valide est enregistrée
void calStart();     // capture le centre + démarre la collecte des extrêmes
void calFinish();    // valide + sauvegarde (NVS)
void calCancel();    // abandonne la calibration en cours
int  calState();     // 0 = inactif, 1 = collecte des extrêmes en cours

} // namespace input
