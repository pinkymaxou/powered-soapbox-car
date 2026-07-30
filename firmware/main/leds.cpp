// leds.cpp — Task dedicated to the WS2812B strip: reflects the kart's state.
//   green = armed & ready · yellow = healthy but disarmed · pulsing yellow = arming ·
//   fault stop = 2 s rapid red blink then a steady 1 Hz red blink until the fault clears ·
//   orange = low battery · blue = calibration.
#include "leds.hpp"

#include "config.hpp"
#include "evlog.hpp"
#include "input.hpp"
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

    // Calibration collection in progress → BLUE, checked FIRST. The truth lives in
    // input::calState(), not in the core's State: the core deliberately knows nothing about
    // the calibration wizard (State::Calibrate exists in the enum but nothing produces it —
    // the blue LED was dead code until this check). It also outranks Fault below: collecting
    // raises the blocking NOCAL on purpose (see input.cpp), and blinking red at someone
    // following the wizard would read as "something broke".
    if (1 == input::calState())
    {
        m_strip.setBrightness(static_cast<uint8_t>(cfg.led_brightness));
        m_strip.setAll(0, 0, 255);
        m_strip.show();
        return;
    }

    switch (state)
    {
        case State::Fault:
            // Stop due to a fault: 2 s rapid red blink to grab attention, then a steady ~1 Hz
            // red blink for as long as the fault lasts. It must NEVER settle to solid red — a
            // static light reads as "state shown", a blinking one as "something needs fixing",
            // and some faults (encoders) latch until reboot with nothing else to signal them.
            r = (m_fault_ms <= FAULT_ALERT_MS) ? (veryfast ? 255 : 0) : (slow ? 255 : 0);
            break;
        case State::Calibrate:
            b = 255;
            break;
        case State::Run:
        {
            // Warning threshold according to the detected battery (12/24 V); unknown type → no
            // alert. Silent too when the voltage check is off (vbat_check_en=0): a bench with
            // nothing on the divider bridge would otherwise sit permanently orange.
            const int   bt     = st.m_batt_type;
            const float warn_v = (24 == bt) ? hw::VBAT24_WARN_V : hw::VBAT12_WARN_V;
            // m_vbat > 0 guard: when the ADS1115 dies mid-drive the telemetry voltage drops
            // to 0 ("unknown"), which is below every warn threshold — without the guard the
            // strip went orange claiming "low battery" when the truth was "no reading".
            if ((0 != bt) && (0 != cfg.vbat_check_en) && st.m_vbat > 0.05f && st.m_vbat < warn_v)
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
        // Piggy-backed housekeeping: drain the event log's RAM ring to flash (disarmed
        // only, no-op when empty). Hosted here instead of a dedicated task on purpose —
        // that task's 3 KB stack helped starve the heap to 864 bytes free at page load.
        evlog::maintain();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(REFRESH_MS));
    }
}
} // namespace

void ledsStart()
{
    xTaskCreatePinnedToCore(ledsTask, rtos::LEDS.name, rtos::LEDS.stack, nullptr,
                            rtos::LEDS.prio, nullptr, rtos::LEDS.core);
}
