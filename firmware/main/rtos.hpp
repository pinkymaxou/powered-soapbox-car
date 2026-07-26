// rtos.hpp — FreeRTOS task parameters (priority, core, stack) gathered in a single place.
// Source of truth referenced by controller.cpp and leds.cpp. Overview (including the
// IDF framework tasks): doc/firmware-tasks.md.
//
// ESP32: 2 cores — core 0 = PRO_CPU (network/system), core 1 = APP_CPU (application).
// FreeRTOS at 1000 Hz; priorities 0 (idle) .. 24 (max), a higher number = higher priority.
#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace rtos
{

struct TaskCfg
{
    const char* name;    // FreeRTOS name (visible in the monitor / watchdog)
    uint32_t    stack;   // stack size in bytes
    UBaseType_t prio;    // 0 (idle) .. 24 (max)
    BaseType_t  core;    // 0 = PRO_CPU (network/system), 1 = APP_CPU (application)
};

// 500 Hz control loop — isolated on the application core (1), HIGH priority so bursty core-1
// host tasks (BT stack / Bluepad32 callbacks) can't stall it past the encoder's ½-turn window
// (which would alias the absolute AS5600 angle). Below esp_timer(22)/Wi-Fi/BT(23)/IPC(24).
// It yields every cycle (vTaskDelayUntil), so the high priority starves nothing. WDT 5 s.
constexpr TaskCfg CONTROL{"control", 6144, 18, 1};

// WS2812B strip display (~20 Hz) — network/system core, low priority.
constexpr TaskCfg LEDS{"leds", 3072, 3, 0};

} // namespace rtos
