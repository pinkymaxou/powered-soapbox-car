// hardware.hpp — Accès matériel bas niveau (LED, ADC, moteurs, encodeurs, boutons).
// Fonctions libres dans le namespace `board` ; tout est initialisé par board::init().
#pragma once

#include <cstdint>

namespace board
{

void init();   // initialise LED, ADC, moteurs (LEDC+DIR), capteur d'angle AS5600 (I2C), boutons, sorties

// LED d'état (onboard)
void led(bool on);
void ledToggle();

// Lectures analogiques
int   throttleRaw(int oversample);   // brut ADC de l'accélérateur
float vbatVolts(int oversample);     // tension à la broche (AVANT le ratio du diviseur)

// Moteurs (l, r ∈ [-1..1] ; cap = duty max = plafond PWM)
void motorsSet(float l, float r, uint32_t cap);
void motorsBrake(float strength, uint32_t cap);   // frein électrique (plugging)
void motorsStop();

// Capteur d'angle (AS5600) : Δcounts signé (12 bits, 4096/tour) depuis le dernier appel.
// encRightDelta() = 0 (réserve, 2e bus I2C non câblé).
int encLeftDelta();
int encRightDelta();

// Boutons : échantillonner pollButtons() une fois par tick (anti-rebond),
// puis lire l'état débruité via btnStart()/btnReverse()/btnCal().
void pollButtons();
bool btnStart();
bool btnReverse();

// Sorties annexes
void reverseLED(bool on);

// Maintien d'alimentation (latch). powerLatch() : à appeler le plus tôt possible au boot
// pour que l'ESP tienne sa propre alimentation après le relâché du bouton externe.
void powerLatch();
void powerOff();

} // namespace board
