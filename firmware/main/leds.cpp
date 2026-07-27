// leds.cpp — Task dedicated to the WS2812B strip: reflects the kart's state.
//   green = armed & ready · yellow = healthy but disarmed · pulsing yellow = arming ·
//   fault stop = 2 s rapid red blink then solid red · orange = low battery · blue = calibration.
#include "leds.hpp"

#include "config.hpp"
#include "pinout.hpp"
#include "rtos.hpp"
#include "ws2812.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
constexpr int REFRESH_MS = 50;             // ~20 Hz
constexpr int FAULT_ALERT_MS = 2000;       // rapid red blink burst on a fault stop
Ws2812 m_strip;
int     m_phase = 0;
int     m_fault_ms = 0;                    // time in Fault since entry (drives the 2 s alert)

void render(const KartConfig& cfg)
{
    ++m_phase;
    const bool slow     = ((m_phase / 10) & 1);   // ~1 Hz
    const bool veryfast = ((m_phase /  2) & 1);    // ~5 Hz (rapid fault alert)
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    const KartStatus st = statusSnapshot();
    const State state = static_cast<State>(st.m_state);
    m_fault_ms = (State::Fault == state) ? (m_fault_ms + REFRESH_MS) : 0;

    switch (state)
    {
        case State::Fault:
            // Stop due to a fault: 2 s rapid red blink to grab attention, then hold solid
            // red while the fault persists (encoder faults latch until reboot).
            r = (m_fault_ms <= FAULT_ALERT_MS) ? (veryfast ? 255 : 0) : 255;
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
                g = 255;       // armed & ready: green
            }
            break;
        }
        case State::Lockout:
        default:
            // Healthy but disarmed: yellow (pulsing while the START hold is in progress).
            if (!st.m_arming || slow)
            {
                r = 255;
                g = 180;       // yellow
            }
            break;
    }

    m_strip.setBrightness(static_cast<uint8_t>(cfg.led_brightness));
    m_strip.setAll(r, g, b);
    m_strip.show();
}

void ledsTask(void*)
{
    const KartConfig cfg = configSnapshot();
    m_strip.init(pins::WS2812, cfg.led_count, static_cast<uint8_t>(cfg.led_brightness));
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
