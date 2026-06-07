// pinout.hpp — Brochage matériel (fixe) du kart électrique 2 places.
// ESP-IDF 6.1 / C++. Les réglages logiciels sont dans config.hpp (table PARAMS).
//
// Notes :
//  - Pas d'entrée frein : relâcher l'accélérateur déclenche le frein électrique.
//  - Pas d'entrée e-stop : l'arrêt d'urgence coupe le courant de TOUT le système
//    (y compris l'ESP32). Au retour du courant, l'ESP redémarre désarmé.
#pragma once

#include "driver/gpio.h"
#include "hal/adc_types.h"

namespace pins
{

// Sorties moteur (vers driver double canal : PWM + DIR par canal)
constexpr gpio_num_t PWM_L = GPIO_NUM_25;
constexpr gpio_num_t DIR_L = GPIO_NUM_26;
constexpr gpio_num_t PWM_R = GPIO_NUM_32;
constexpr gpio_num_t DIR_R = GPIO_NUM_33;

// Entrées analogiques (ADC1 uniquement — ADC2 entre en conflit avec le Wi-Fi)
constexpr adc_channel_t THROTTLE = ADC_CHANNEL_6;  // GPIO34 : curseur du potentiomètre
constexpr adc_channel_t VBAT     = ADC_CHANNEL_3;  // GPIO39 : pont diviseur 100k/15k

// Boutons (momentanés, pull-up interne, actifs à l'état bas)
constexpr gpio_num_t START_BTN   = GPIO_NUM_16;
constexpr gpio_num_t REVERSE_BTN = GPIO_NUM_21;
// (calibration via le web uniquement — pas de bouton CAL)

// Maintien d'alimentation (latch). Commande ACTIVE BASSE : cette sortie tire vers la masse
// la LED d'un optocoupleur ALIMENTÉE PAR LE 3,3 V de l'ESP. Le transistor de l'opto maintient
// alors le +20 V sur la gate des MOSFET (le +20 V ne remonte jamais à l'ESP).
// L'AMORÇAGE se fait par un bouton externe qui met le +20 V directement sur la gate (le 3,3 V
// n'existe pas encore au démarrage). Une fois alimenté, l'ESP « se tient en vie » en gardant
// cette sortie BASSE et coupe en la relâchant (HAUT) — p. ex. batterie trop basse.
// L'arrêt d'urgence reste EN SÉRIE dans la ligne de gate (hors MCU).
constexpr gpio_num_t POWER_HOLD  = GPIO_NUM_13;  // sortie ; BAS = système maintenu, HAUT = coupe

// Capteur d'angle AS5600 sur I2C (3,3 V natif → branchement direct, AUCUN level-shift).
// Adresse fixe 0x36 → un seul capteur par bus. Connecteur 4 fils : SDA, SCL, 3V3, GND
// (+ aimant diamétral en bout d'essieu). Pull-ups 4,7 kΩ sur SDA/SCL.
// GPIO27 / GPIO14 restent libres (réserve : 2e bus I2C pour un 2e capteur).
constexpr gpio_num_t I2C_SDA = GPIO_NUM_18;
constexpr gpio_num_t I2C_SCL = GPIO_NUM_19;

// Sorties
constexpr gpio_num_t LED         = GPIO_NUM_2;
constexpr gpio_num_t WS2812      = GPIO_NUM_17;
constexpr gpio_num_t REVERSE_LED = GPIO_NUM_4;

// Niveaux actifs
constexpr int BTN_ACTIVE = 0;   // boutons pressés = niveau bas

} // namespace pins
