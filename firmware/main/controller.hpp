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
    void tickOnce();   // one complete step: pushed inputs → tick() → host decisions → publication

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
    bool           m_was_armed = false;   // armed→disarmed edge → configFlushPending
};

// ESP-side bootstrap: owns the EspController instance, creates the 500 Hz control task
// (5 s watchdog) that calls tickOnce() in a loop. Nothing else.
namespace Controller
{
void init();    // to call after configInit
void start();
} // namespace Controller
