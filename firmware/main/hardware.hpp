// hardware.hpp — Low-level hardware access (LED, ADC, motors, encoders, buttons).
// Free functions in the `board` namespace; everything is initialized by board::init().
#pragma once

#include <cstdint>

namespace board
{

void init();   // initializes LED, motors (LEDC+DIR), 2× AS5600 (I2C), ADS1115 (Vbat), START button

// Status LED (onboard)
void led(bool on);

// Analog reading (via external ADS1115 ADC) — voltage at pin A0 (BEFORE the divider ratio).
float vbatVolts(int oversample);     // oversample = number of readings averaged

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

// START button: pollButtons() once per tick (debounce), then btnStart().
void pollButtons();
bool btnStart();

// Call at the VERY START of boot: forces the PWM/DIR pins to the low level (motors stopped)
// before full init, to prevent any spurious movement while the GPIOs float.
void motorsIdleEarly();

// Power latch. powerLatch(): call as early as possible at boot
// so the ESP holds its own power after the external button is released.
void powerLatch();
void powerOff();

} // namespace board
