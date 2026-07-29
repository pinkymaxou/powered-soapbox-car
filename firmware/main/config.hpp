// config.hpp — ESP-side configuration: PARAMS table (NVS/web key → field), persistence,
// shared telemetry (KartStatus, atomics) and thread-safe access. The pure control TYPES
// (KartConfig, enums, hw:: constants) live in control_types.hpp (shared with the host).
#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "control_types.hpp"

struct KartStatus
{
    // SIMPLE struct (no more atomics): read by COPY under mutex via statusSnapshot(),
    // written by statusPublish() — a single writer (the control loop).
    int      m_state = static_cast<int>(State::Lockout);
    int      m_fault = static_cast<int>(Fault::None);
    unsigned m_faults = 0;    // mask of ACTIVE conditions (Faults page)
    float    m_vbat = 0.f;
    int      m_batt_type = 0; // battery detected at startup: 0 = in progress, 12 or 24 (V)
    float    m_rpm_l = 0.f;   // SIGNED front left wheel (AS5600 #1, rpm)
    float    m_rpm_r = 0.f;
    float    m_speed_ms = 0.f;// SIGNED VEHICLE speed (m/s) — pivot in place → 0
    float    m_fwd = 0.f;     // forward command after mix/limits [-1..1]
    float    m_turn = 0.f;    // turn command after rollover protection [-1..1]
    bool     m_btn_start = false;
    float    m_out_l = 0.f;   // left motor PWM [-1..1]
    float    m_out_r = 0.f;
    int      m_brake_mode = static_cast<int>(BrakeMode::Dynamic);
    bool     m_arming = false;
    bool     m_estop = false;
    bool     m_pad_conn = false;
    int      m_pad_batt = -1;   // gamepad battery 0..100 (-1 unknown)
    float    m_pad_x = 0.f;     // RAW turn stick [-1..1] (physical position, circle)
    float    m_pad_y = 0.f;
    float    m_pad_cx = 0.f;    // COMPENSATED turn command circle→square [-1..1]
    float    m_pad_cy = 0.f;
    float    m_pad_zl = 0.f;    // analog triggers [0..1] (display)
    float    m_pad_zr = 0.f;
    float    m_pad_rx2 = 0.f;   // RIGHT stick [-1..1] (display only)
    float    m_pad_ry2 = 0.f;
    unsigned m_pad_btns = 0;    // mask: buttons | (misc<<16) | (dpad<<24) (display)
    int      m_idle_off_s = -1; // seconds before the idle power-off (-1 = not counting)
};

// PROTECTED access to the telemetry (internal mutex): coherent copy for the readers
// (webserver, LED), atomic write of a block for the control loop.
KartStatus statusSnapshot();
bool       statusTrySnapshot(KartStatus& out);   // timeout 0 — for the esp_timer callbacks
void       statusPublish(const KartStatus& s);

// ───────────────────────── Globals ─────────────────────────
extern KartConfig        g_cfg;
extern SemaphoreHandle_t g_cfg_mtx;

void       configInit();
bool       configLoad();
bool       configSave();
KartConfig configSnapshot();
void       configUpdate(const KartConfig& c, bool persist);
void       configFlushPending();   // persists a deferred "set" (to call once disarmed)

bool configGetWifi(char* ssid, size_t ssid_size, char* pass, size_t pass_size, bool* enabled = nullptr);
void configSetWifi(const char* ssid, const char* pass, bool enabled);
