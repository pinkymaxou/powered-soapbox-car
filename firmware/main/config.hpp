// config.hpp — Configuration côté ESP : table PARAMS (clé NVS/web → champ), persistance,
// télémétrie partagée (KartStatus, atomics) et accès thread-safe. Les TYPES purs du contrôle
// (KartConfig, enums, constantes hw::) vivent dans control_types.hpp (partagés avec l'hôte).
#pragma once

#include <atomic>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "control_types.hpp"

struct KartStatus
{
    std::atomic<int>   m_state{static_cast<int>(State::Lockout)};
    std::atomic<int>   m_fault{static_cast<int>(Fault::None)};
    std::atomic<unsigned> m_faults{0};   // masque des conditions ACTIVES (page Défauts) — voir controller.cpp
    std::atomic<float> m_vbat{0.f};
    std::atomic<int>   m_batt_type{0};   // batterie détectée au démarrage : 0 = en cours, 12 ou 24 (V)
    std::atomic<float> m_speed_l{0.f};   // vitesse roue avant gauche SIGNÉE (AS5600 #1, m/s)
    std::atomic<float> m_speed_r{0.f};   // vitesse roue avant droite SIGNÉE (AS5600 #2, m/s)
    std::atomic<float> m_speed_ms{0.f};  // vitesse VÉHICULE signée (m/s) = (vG+vD)/2 — pivot sur place → 0
    std::atomic<float> m_fwd{0.f};       // consigne d'avance après mix/limites [-1..1]
    std::atomic<float> m_turn{0.f};      // consigne de virage après anti-renversement [-1..1]
    std::atomic<bool>  m_btn_start{false};
    std::atomic<float> m_out_l{0.f};     // PWM moteur gauche [-1..1]
    std::atomic<float> m_out_r{0.f};     // PWM moteur droite [-1..1]
    std::atomic<int>   m_brake_mode{static_cast<int>(BrakeMode::Dynamic)};   // BrakeMode effectif
    std::atomic<bool>  m_arming{false};
    std::atomic<bool>  m_estop{false};
    std::atomic<bool>  m_pad_conn{false}; // manette connectée
    std::atomic<int>   m_pad_batt{-1};    // batterie manette 0..100 (-1 inconnu)
    std::atomic<float> m_pad_x{0.f};      // stick BRUT virage [-1..1] (position physique, cercle)
    std::atomic<float> m_pad_y{0.f};      // stick BRUT avance [-1..1] (position physique, cercle)
    std::atomic<float> m_pad_cx{0.f};     // consigne virage COMPENSÉE cercle→carré [-1..1]
    std::atomic<float> m_pad_cy{0.f};     // consigne avance COMPENSÉE cercle→carré [-1..1]
    std::atomic<float> m_pad_zl{0.f};     // gâchette analogique gauche ZL [0..1] (affichage)
    std::atomic<float> m_pad_zr{0.f};     // gâchette analogique droite ZR [0..1] (affichage)
    std::atomic<float> m_pad_rx2{0.f};    // stick DROIT X [-1..1] (affichage seulement)
    std::atomic<float> m_pad_ry2{0.f};    // stick DROIT Y [-1..1] (affichage seulement)
    std::atomic<unsigned> m_pad_btns{0};  // masque : boutons | (misc<<16) | (dpad<<24) (affichage)
};

// ───────────────────────── Globaux ─────────────────────────
extern KartConfig        g_cfg;
extern KartStatus        g_status;
extern SemaphoreHandle_t g_cfg_mtx;

void       configInit();
bool       configLoad();
bool       configSave();
KartConfig configSnapshot();
void       configUpdate(const KartConfig& c, bool persist);
void       configFlushPending();   // persiste un « set » différé (à appeler une fois désarmé)

bool configGetWifi(char* ssid, size_t ssid_size, char* pass, size_t pass_size, bool* enabled = nullptr);
void configSetWifi(const char* ssid, const char* pass, bool enabled);
