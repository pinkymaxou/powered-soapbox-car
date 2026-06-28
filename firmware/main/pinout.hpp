// pinout.hpp — Brochage matériel (fixe). Variante EXPÉRIMENTALE : tricycle à entraînement
// différentiel — 2 roues AVANT motrices indépendantes + 1 roulette arrière folle.
// Pilotage par manette Bluetooth (aucune entrée pédale). ESP-IDF 6.1 / C++.
#pragma once

#include "driver/gpio.h"

namespace pins
{

// Sorties moteur (driver double canal : PWM + DIR par canal) — un moteur par roue avant.
constexpr gpio_num_t PWM_L = GPIO_NUM_25;   // roue avant GAUCHE
constexpr gpio_num_t DIR_L = GPIO_NUM_26;
constexpr gpio_num_t PWM_R = GPIO_NUM_32;   // roue avant DROITE
constexpr gpio_num_t DIR_R = GPIO_NUM_33;

// Bouton d'armement (momentané, pull-up interne, actif bas). Pilotage = manette (pas de pédale).
constexpr gpio_num_t START_BTN = GPIO_NUM_16;

// Maintien d'alimentation (latch). Commande ACTIVE BASSE (voir doc : opto + MOSFET low-side).
constexpr gpio_num_t POWER_HOLD = GPIO_NUM_13;  // BAS = système maintenu, HAUT = coupe

// ───────────────────────── Bus I2C (deux bus indépendants) ─────────────────────────
// Toutes les mesures analogiques passent par l'ADC externe ADS1115 (16 bits) au lieu de
// l'ADC interne de l'ESP32 (plus précis, et l'ADC2 entrait en conflit avec le Wi-Fi).
// Bus 0 partagé : AS5600 roue gauche (0x36) + ADS1115 (0x48) — adresses distinctes, OK.
constexpr gpio_num_t I2C0_SDA = GPIO_NUM_18;   // bus 0 → AS5600 roue GAUCHE (0x36) + ADS1115 (0x48)
constexpr gpio_num_t I2C0_SCL = GPIO_NUM_19;
constexpr gpio_num_t I2C1_SDA = GPIO_NUM_27;   // bus 1 → AS5600 roue DROITE (0x36)
constexpr gpio_num_t I2C1_SCL = GPIO_NUM_14;
// 3,3 V natif (aucun level-shift) ; pull-ups 4,7 kΩ par paire SDA/SCL.

// ───────────────────── Entrées analogiques (canaux ADS1115, A0..A3) ─────────────────────
// ADS1115 alimenté en 3,3 V (⇒ AIN_max = 3,3 V). Single-ended par rapport à GND.
namespace ads
{
constexpr uint8_t VBAT = 0;   // A0 : tension batterie via pont diviseur 100k/15k (suivi en continu)
// ── Réserve : joystick physique analogique (FUTUR, non câblé/non utilisé) ──
// Le pilotage actuel est 100 % manette Bluetooth ; on réserve 2 voies de l'ADS1115 pour
// brancher plus tard un joystick X/Y derrière la même abstraction `input` (lecture single-shot).
constexpr uint8_t JOY_X = 1;  // A1 — virage  (futur)
constexpr uint8_t JOY_Y = 2;  // A2 — avance  (futur)
// A3 : libre.
} // namespace ads

// Sorties d'état
constexpr gpio_num_t LED    = GPIO_NUM_2;    // LED carte
constexpr gpio_num_t WS2812 = GPIO_NUM_17;   // ruban d'état

// Niveaux actifs
constexpr int BTN_ACTIVE = 0;   // bouton pressé = niveau bas

// ───── Réserve : encodeurs incrémentaux en quadrature AMT103-V (FUTUR, non câblé) ─────
// « Au cas où » : alternative/complément aux AS5600 (un encodeur par roue avant). Sorties
// CMOS push-pull A/B (+ index X en option) ; décodage matériel via le périphérique PCNT.
// ⚠️ Alimentation : VDD min ~3,6 V → sortie haute ≈ VDD−0,8 ≈ 2,8 V, lisible par l'ESP32
//    SANS level-shift. À 5 V la sortie monte à ~4,2 V → diviseur/level-shift OBLIGATOIRE.
// Broches INPUT-ONLY (34/35/36/39) : idéales en entrée (signaux pilotés, aucun pull-up requis).
namespace future
{
constexpr gpio_num_t ENC_L_A = GPIO_NUM_34;   // encodeur roue GAUCHE — canal A
constexpr gpio_num_t ENC_L_B = GPIO_NUM_35;   // encodeur roue GAUCHE — canal B
constexpr gpio_num_t ENC_R_A = GPIO_NUM_36;   // encodeur roue DROITE — canal A
constexpr gpio_num_t ENC_R_B = GPIO_NUM_39;   // encodeur roue DROITE — canal B
// Index (X, 1 impulsion/tour) optionnel : à câbler sur 22/23 si besoin plus tard.
} // namespace future

// Autres GPIO libres : 4, 21, 22, 23.

} // namespace pins
