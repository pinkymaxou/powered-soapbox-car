// hardware.hpp — Low-level hardware access (LED, motors, encoders).
// Free functions in the `board` namespace; everything is initialized by board::init().
#pragma once

#include <cstdint>

namespace board
{

void init();   // initializes LED, motors (LEDC+DIR), 2× AS5600 (I2C)

// Status LED (onboard)
void led(bool on);

// Motors (l, r ∈ [-1..1], independent; cap = max duty = PWM ceiling)
void motorsSet(float l, float r, uint32_t cap);
// Dynamic braking: short-circuits the motors (low outputs) → resists movement.
// DEFAULT state of the controller at rest (rather than coasting).
void motorsBrake();

// AS5600 angle sensors (one per front wheel): signed Δcounts (12 bits) since the last call.
int encLeftDelta();    // front left wheel   (I2C bus 0)
int encRightDelta();   // front right wheel  (I2C bus 1)
uint32_t ledcClkFixCount();   // number of LEDC clock-gate repairs (DPORT anti-race sentinel)
bool encLeftPresent();   // last I2C read of the left AS5600 succeeded
bool encRightPresent();  // same, right
bool encLeftMagOk();     // AS5600 STATUS: left magnet properly in field (MD, not too weak/strong)
bool encRightMagOk();    // same, right
void refreshMagStatus(); // poll the AS5600 STATUS register (rate-limited); call once per control tick

// Call at the VERY START of boot: forces the PWM/DIR pins to the low level (motors stopped)
// before full init, to prevent any spurious movement while the GPIOs float.
// (There is no power latch any more: the main relay is a hardware affair — main switch and
// e-stop mushroom in its coil loop — and the firmware neither holds nor cuts its own supply.)
void motorsIdleEarly();

} // namespace board
