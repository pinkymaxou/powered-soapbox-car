// pinout.hpp — Hardware pinout (fixed). EXPERIMENTAL variant: differential-drive
// tricycle — 2 independent driven REAR wheels + 1 free FRONT caster (reversed layout:
// the single wheel leads, the mass sits over it, and the pair drives under the bench).
// Driven by Bluetooth gamepad (no pedal input). ESP-IDF 6.1 / C++.
//
// SIMPLIFIED ELECTRICAL ARCHITECTURE (2026-09-20): ONE main relay carries the whole kart
// (logic AND motors) and its coil runs through the e-stop mushroom — see doc/electronique.md.
// The firmware neither commands nor senses that relay: pressing the mushroom cuts the ESP
// with everything else, so there is no power-latch pin and no coil-sense pin any more.
// The pack is ALWAYS 12 V: no divider, no ADS1115, no analog input at all.
#pragma once

#include "driver/gpio.h"

namespace pins
{

// Motor outputs (dual-channel driver: PWM + DIR per channel) — one motor per REAR wheel.
constexpr gpio_num_t PWM_L = GPIO_NUM_26;   // rear LEFT wheel
constexpr gpio_num_t DIR_L = GPIO_NUM_25;
constexpr gpio_num_t PWM_R = GPIO_NUM_33;   // rear RIGHT wheel
constexpr gpio_num_t DIR_R = GPIO_NUM_32;

// Status outputs
constexpr gpio_num_t LED    = GPIO_NUM_2;    // board LED
constexpr gpio_num_t WS2812 = GPIO_NUM_4;   // status strip

// NO BUTTON AT ALL on this board: arming is the gamepad's START/Options button, and power is
// the main switch. The ESP32 has exactly three jobs left — drive two motor channels, talk to
// the gamepad and light the strip. No sensor of any kind (the AS5600 encoders and their two
// I2C buses went away on 2026-09-29).
//
// Free GPIOs: 13, 14, 16, 18, 19, 21, 22, 23, 27 and the input-only 34, 35, 36, 39 (no internal
// pulls on those four). 13 was the power latch, 22 the e-stop coil sense, 16 the arming button,
// 18/19 and 27/14 the two I2C buses of the encoders.

} // namespace pins
