// leds.cpp — Task dedicated to the WS2812B strip: reflects the kart's state.
//   green = armed/running · red = disarmed · pulsing amber = arming in progress ·
//   orange = low battery · blue = calibration · blinking red = fault.
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

    const KartStatus st = statusSnapshot();
    switch (static_cast<State>(st.m_state))
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
        {
            // Warning threshold according to the detected battery (12/24 V); unknown type → no alert.
            const int   bt     = st.m_batt_type;
            const float warn_v = (24 == bt) ? hw::VBAT24_WARN_V : hw::VBAT12_WARN_V;
            if ((0 != bt) && st.m_vbat < warn_v)
            {
                r = 255;
                g = 120;       // low battery: orange
            }
            else if ((static_cast<int>(BrakeMode::None) != st.m_brake_mode) && slow)
            {
                g = 255;       // braking: blinking green
            }
            else
            {
                g = 255;       // armed: green
            }
            break;
        }
        case State::Lockout:
        default:
            if (st.m_arming)
            {
                if (slow)
                {
                    r = 255;
                    g = 120;   // arming in progress: pulsing amber
                }
            }
            else
            {
                r = 255;       // disarmed: red
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
