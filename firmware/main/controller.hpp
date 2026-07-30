// controller.hpp — HARDWARE binding of the control core. EspController fills the two
// KartController callbacks (sensors ← board::, motor outputs → board::) and pushes it
// the inputs (gamepad, START button, web config); ALL the business logic is in the
// core. The host decisions (rumble, power cutoff, deferred persistence) are
// derived from the telemetry via advisors.hpp. The Controller namespace is only the BOOTSTRAP:
// it initializes the instance, creates the 500 Hz FreeRTOS task and runs tickOnce() in it.
#pragma once

#include "advisors.hpp"
#include "controller_core.hpp"
#include "input.hpp"

class EspController
{
public:
    void init();       // hardware (board/input) + wiring of the core callbacks
    void tickOnce();                  // one complete step: inputs → tick() → host decisions → publish
    uint32_t loopMaxUs(bool reset);   // worst tick duration (µs) since the last call
    uint32_t sensMaxUs(bool reset);   // worst SENSOR-READ duration (µs) — I2C cost alone

private:
    SensorReadings readSensors();                 // sensor callback: encoders + Vbat (in battery VOLTS)
    void           applyOutputs(const CtrlOutputs& out);   // output callback: motor command
    void           pushPad();                     // pushes the gamepad state to the core (setPad)
    void           publish(const CtrlTelemetry& t);   // telemetry + gamepad display → statusPublish

    KartController m_ctrl;       // the PURE logic (identical in simulation)
    input::State   m_in;         // last complete gamepad snapshot (display fields)
    PadInputs      m_pad_in;     // last gamepad state pushed to the core (for the advisors)
    RumbleAdvisor  m_rumble;     // haptic feedback (host decision)
    PowerOffAdvisor m_poweroff;  // power cutoff on prolonged LVC (host decision)
    IdleOffAdvisor m_idle_off;   // power cutoff after N minutes disarmed (host decision)
    bool           m_was_armed = false;   // armed→disarmed edge → configFlushPending
    uint32_t       m_loop_max_us = 0;     // worst tick duration since the last read
    uint32_t       m_sens_max_us = 0;     // worst readSensors() duration
    int            m_vbat_tick = 0;        // rate-limit the ADS1115 read (shares bus 0 w/ left enc)
    float          m_pin_v = -1.f;         // last ADC pin voltage (cached between 20 Hz reads)
};

// ESP-side bootstrap: owns the EspController instance, creates the 500 Hz control task
// (5 s watchdog) that calls tickOnce() in a loop. Nothing else.
namespace Controller
{
uint32_t loopMaxUs();   // worst tick (µs) since the last call — diagnostic, resets on read
uint32_t sensMaxUs();   // worst sensor-read (µs) — separates I2C cost from preemption
void init();    // to call after configInit
void start();
} // namespace Controller
