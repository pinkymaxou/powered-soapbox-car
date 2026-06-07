// rtos.hpp — Paramètres des tâches FreeRTOS (priorité, cœur, pile) regroupés en un seul endroit.
// Source de vérité référencée par controller.cpp et leds.cpp. Vue d'ensemble (y compris les
// tâches du framework IDF) : doc/firmware-tasks.md.
//
// ESP32 : 2 cœurs — cœur 0 = PRO_CPU (réseau/système), cœur 1 = APP_CPU (applicatif).
// FreeRTOS à 1000 Hz ; priorités 0 (idle) .. 24 (max), un nombre plus élevé = plus prioritaire.
#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace rtos
{

struct TaskCfg
{
    const char* name;    // nom FreeRTOS (visible au monitor / watchdog)
    uint32_t    stack;   // taille de pile en octets
    UBaseType_t prio;    // 0 (idle) .. 24 (max)
    BaseType_t  core;    // 0 = PRO_CPU (réseau/système), 1 = APP_CPU (applicatif)
};

// Boucle d'asservissement 500 Hz — isolée sur le cœur applicatif, prioritaire, abonnée au watchdog 5 s.
constexpr TaskCfg CONTROL{"control", 6144, 6, 1};

// Affichage du ruban WS2812B (~20 Hz) — cœur réseau/système, basse priorité.
constexpr TaskCfg LEDS{"leds", 3072, 3, 0};

} // namespace rtos
