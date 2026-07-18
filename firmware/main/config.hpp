// config.hpp — Configuration côté ESP : table PARAMS (clé NVS/web → champ), persistance,
// télémétrie partagée (KartStatus, atomics) et accès thread-safe. Les TYPES purs du contrôle
// (KartConfig, enums, constantes hw::) vivent dans control_types.hpp (partagés avec l'hôte).
#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "control_types.hpp"

struct KartStatus
{
    // Struct SIMPLE (plus d'atomics) : lecture par COPIE sous mutex via statusSnapshot(),
    // écriture par statusPublish() — un seul écrivain (la boucle de contrôle).
    int      m_state = static_cast<int>(State::Lockout);
    int      m_fault = static_cast<int>(Fault::None);
    unsigned m_faults = 0;    // masque des conditions ACTIVES (page Défauts)
    float    m_vbat = 0.f;
    int      m_batt_type = 0; // batterie détectée au démarrage : 0 = en cours, 12 ou 24 (V)
    float    m_rpm_l = 0.f;   // roue avant gauche SIGNÉE (AS5600 #1, tr/min)
    float    m_rpm_r = 0.f;
    float    m_speed_ms = 0.f;// vitesse VÉHICULE signée (m/s) — pivot sur place → 0
    float    m_fwd = 0.f;     // consigne d'avance après mix/limites [-1..1]
    float    m_turn = 0.f;    // consigne de virage après anti-renversement [-1..1]
    bool     m_btn_start = false;
    float    m_out_l = 0.f;   // PWM moteur gauche [-1..1]
    float    m_out_r = 0.f;
    int      m_brake_mode = static_cast<int>(BrakeMode::Dynamic);
    bool     m_arming = false;
    bool     m_estop = false;
    bool     m_pad_conn = false;
    int      m_pad_batt = -1;   // batterie manette 0..100 (-1 inconnu)
    float    m_pad_x = 0.f;     // stick BRUT virage [-1..1] (position physique, cercle)
    float    m_pad_y = 0.f;
    float    m_pad_cx = 0.f;    // consigne virage COMPENSÉE cercle→carré [-1..1]
    float    m_pad_cy = 0.f;
    float    m_pad_zl = 0.f;    // gâchettes analogiques [0..1] (affichage)
    float    m_pad_zr = 0.f;
    float    m_pad_rx2 = 0.f;   // stick DROIT [-1..1] (affichage seulement)
    float    m_pad_ry2 = 0.f;
    unsigned m_pad_btns = 0;    // masque : boutons | (misc<<16) | (dpad<<24) (affichage)
};

// Accès PROTÉGÉ à la télémétrie (mutex interne) : copie cohérente pour les lecteurs
// (webserver, LED), écriture atomique d'un bloc pour la boucle de contrôle.
KartStatus statusSnapshot();
bool       statusTrySnapshot(KartStatus& out);   // timeout 0 — pour les callbacks esp_timer
void       statusPublish(const KartStatus& s);

// ───────────────────────── Globaux ─────────────────────────
extern KartConfig        g_cfg;
extern SemaphoreHandle_t g_cfg_mtx;

void       configInit();
bool       configLoad();
bool       configSave();
KartConfig configSnapshot();
void       configUpdate(const KartConfig& c, bool persist);
void       configFlushPending();   // persiste un « set » différé (à appeler une fois désarmé)

bool configGetWifi(char* ssid, size_t ssid_size, char* pass, size_t pass_size, bool* enabled = nullptr);
void configSetWifi(const char* ssid, const char* pass, bool enabled);
