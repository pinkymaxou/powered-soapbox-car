// leds.cpp — Tâche dédiée au ruban WS2812B : reflète l'état du kart.
//   vert = armé/en route · rouge = désarmé · ambre pulsé = armement en cours ·
//   orange = batterie faible · bleu = calibration · rouge clignotant = défaut.
#include "leds.hpp"

#include "config.hpp"
#include "pinout.hpp"
#include "rtos.hpp"
#include "ws2812.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
constexpr int REFRESH_MS = 50;   // ~20 Hz
Ws2812 m_strip;
int     m_phase = 0;

void render(const KartConfig& cfg)
{
    ++m_phase;
    const bool slow = ((m_phase / 10) & 1);   // ~1 Hz
    const bool fast = ((m_phase / 3) & 1);    // ~3 Hz
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    switch (static_cast<State>(g_status.m_state.load()))
    {
        case State::Fault:
            if (fast)
            {
                r = 255;
            }
            break;
        case State::Calibrate:
            b = 255;
            break;
        case State::Run:
            if (g_status.m_vbat.load() < cfg.vbat_warn_v)
            {
                r = 255;
                g = 120;       // batterie faible : orange
            }
            else if (g_status.m_brake.load() && slow)
            {
                g = 255;       // freinage : vert clignotant
            }
            else
            {
                g = 255;       // armé : vert
            }
            break;
        case State::Lockout:
        default:
            if (g_status.m_arming.load())
            {
                if (slow)
                {
                    r = 255;
                    g = 120;   // armement en cours : ambre pulsé
                }
            }
            else
            {
                r = 255;       // désarmé : rouge
            }
            break;
    }

    m_strip.setBrightness(static_cast<uint8_t>(iround(cfg.led_brightness)));
    m_strip.setAll(r, g, b);
    m_strip.show();
}

void ledsTask(void*)
{
    const KartConfig cfg = configSnapshot();
    m_strip.init(pins::WS2812, iround(cfg.led_count), static_cast<uint8_t>(iround(cfg.led_brightness)));
    TickType_t last = xTaskGetTickCount();
    while (true)
    {
        render(configSnapshot());
        vTaskDelayUntil(&last, pdMS_TO_TICKS(REFRESH_MS));
    }
}
} // namespace

void ledsStart()
{
    xTaskCreatePinnedToCore(ledsTask, rtos::LEDS.name, rtos::LEDS.stack, nullptr,
                            rtos::LEDS.prio, nullptr, rtos::LEDS.core);
}
