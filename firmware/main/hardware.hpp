// hardware.hpp — Accès matériel bas niveau (LED, ADC, moteurs, encodeurs, boutons).
// Fonctions libres dans le namespace `board` ; tout est initialisé par board::init().
#pragma once

#include <cstdint>

namespace board
{

void init();   // initialise LED, moteurs (LEDC+DIR), 2× AS5600 (I2C), ADS1115 (Vbat), bouton START

// LED d'état (onboard)
void led(bool on);
void ledToggle();

// Lecture analogique (via ADC externe ADS1115) — tension à la broche A0 (AVANT le ratio du diviseur).
float vbatVolts(int oversample);     // oversample = nb de lectures moyennées

// Moteurs (l, r ∈ [-1..1], indépendants ; cap = duty max = plafond PWM)
void motorsSet(float l, float r, uint32_t cap);
void motorsStop();
// Freinage dynamique : court-circuite les moteurs (sorties basses) → résiste au mouvement.
// État PAR DÉFAUT du contrôleur au repos (plutôt que roue libre).
void motorsBrake();

// Capteurs d'angle AS5600 (un par roue avant) : Δcounts signé (12 bits) depuis le dernier appel.
int encLeftDelta();    // roue avant gauche  (bus I2C 0)
int encRightDelta();   // roue avant droite  (bus I2C 1)
bool encLeftPresent();   // dernière lecture I2C du AS5600 gauche réussie
bool encRightPresent();  // idem droite

// Bouton START : pollButtons() une fois par tick (anti-rebond), puis btnStart().
void pollButtons();
bool btnStart();

// À appeler AU TOUT DÉBUT du boot : force les broches PWM/DIR à l'état bas (moteurs à l'arrêt)
// avant l'init complète, pour éviter tout mouvement parasite pendant que les GPIO flottent.
void motorsIdleEarly();

// Maintien d'alimentation (latch). powerLatch() : à appeler le plus tôt possible au boot
// pour que l'ESP tienne sa propre alimentation après le relâché du bouton externe.
void powerLatch();
void powerOff();

} // namespace board
