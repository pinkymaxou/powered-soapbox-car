// hardware.hpp — Low-level hardware access (LED, motors). No sensors on this kart.
// Free functions in the `board` namespace; everything is initialized by board::init().
#pragma once

#include <cstdint>

namespace board
{

void init();   // initializes LED, motors (LEDC+DIR)

// Status LED (onboard)
void led(bool on);

// Motors (l, r ∈ [-1..1], independent; cap = max duty = PWM ceiling)
void motorsSet(float l, float r, uint32_t cap);
// Dynamic braking: short-circuits the motors (low outputs) → resists movement.
// DEFAULT state of the controller at rest (rather than coasting).
void motorsBrake();

uint32_t ledcClkFixCount();   // number of LEDC clock-gate repairs (DPORT anti-race sentinel)

// Call at the VERY START of boot: forces the PWM/DIR pins to the low level (motors stopped)
// before full init, to prevent any spurious movement while the GPIOs float.
// (There is no power latch any more: the main relay is a hardware affair — main switch and
// e-stop mushroom in its coil loop — and the firmware neither holds nor cuts its own supply.)
void motorsIdleEarly();

} // namespace board
