// pinout.hpp — Hardware pinout (fixed). EXPERIMENTAL variant: differential-drive
// tricycle — 2 independent driven FRONT wheels + 1 free rear caster.
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

// Motor outputs (dual-channel driver: PWM + DIR per channel) — one motor per front wheel.
constexpr gpio_num_t PWM_L = GPIO_NUM_25;   // front LEFT wheel
constexpr gpio_num_t DIR_L = GPIO_NUM_26;
constexpr gpio_num_t PWM_R = GPIO_NUM_32;   // front RIGHT wheel
constexpr gpio_num_t DIR_R = GPIO_NUM_33;

// ───────────────────────── I2C buses (two independent buses) ─────────────────────────
// One AS5600 (0x36) per bus: the address is fixed on that chip, so two sensors need two
// buses. Since the ADS1115 went away, bus 0 carries the left sensor ALONE — which is also
// what removed the read-timing interference the battery polling used to add to that bus.
constexpr gpio_num_t I2C0_SDA = GPIO_NUM_18;   // bus 0 → LEFT wheel AS5600 (0x36)
constexpr gpio_num_t I2C0_SCL = GPIO_NUM_19;
constexpr gpio_num_t I2C1_SDA = GPIO_NUM_27;   // bus 1 → RIGHT wheel AS5600 (0x36)
constexpr gpio_num_t I2C1_SCL = GPIO_NUM_14;
// Native 3.3 V (no level-shift); 4.7 kΩ pull-ups per SDA/SCL pair.

// Status outputs
constexpr gpio_num_t LED    = GPIO_NUM_2;    // board LED
constexpr gpio_num_t WS2812 = GPIO_NUM_4;   // status strip

// NO BUTTON AT ALL on this board: arming is the gamepad's START/Options button, and power is
// the main switch. The ESP32 has exactly four jobs left — read two encoders, drive two motor
// channels, talk to the gamepad and light the strip.
//
// Free GPIOs: 13, 16, 21, 22, 23 and the input-only 34, 35, 36, 39 (no internal pulls on
// those four). 13 was the power latch, 22 the e-stop coil sense, 16 the arming button — all
// three removed with the single-relay wiring. (Quadrature encoders were reserved here as a
// fallback to the AS5600; dropped — the magnetic sensors do the job. See doc/reducteur.md.)

} // namespace pins
