// pinout.hpp — Hardware pinout (fixed). EXPERIMENTAL variant: differential-drive
// tricycle — 2 independent driven FRONT wheels + 1 free rear caster.
// Driven by Bluetooth gamepad (no pedal input). ESP-IDF 6.1 / C++.
#pragma once

#include "driver/gpio.h"

namespace pins
{

// Motor outputs (dual-channel driver: PWM + DIR per channel) — one motor per front wheel.
constexpr gpio_num_t PWM_L = GPIO_NUM_25;   // front LEFT wheel
constexpr gpio_num_t DIR_L = GPIO_NUM_26;
constexpr gpio_num_t PWM_R = GPIO_NUM_32;   // front RIGHT wheel
constexpr gpio_num_t DIR_R = GPIO_NUM_33;

// Arming button (momentary, internal pull-up, active low). Control = gamepad (no pedal).
constexpr gpio_num_t START_BTN = GPIO_NUM_16;

// Power latch. ACTIVE LOW command (see doc: opto + low-side MOSFET).
constexpr gpio_num_t POWER_HOLD = GPIO_NUM_13;  // LOW = system held, HIGH = cut

// ───────────────────────── I2C buses (two independent buses) ─────────────────────────
// All analog measurements go through the external ADS1115 ADC (16 bits) instead of
// the ESP32's internal ADC (more precise, and ADC2 conflicted with Wi-Fi).
// Shared bus 0: left-wheel AS5600 (0x36) + ADS1115 (0x48) — distinct addresses, OK.
constexpr gpio_num_t I2C0_SDA = GPIO_NUM_18;   // bus 0 → LEFT wheel AS5600 (0x36) + ADS1115 (0x48)
constexpr gpio_num_t I2C0_SCL = GPIO_NUM_19;
constexpr gpio_num_t I2C1_SDA = GPIO_NUM_27;   // bus 1 → RIGHT wheel AS5600 (0x36)
constexpr gpio_num_t I2C1_SCL = GPIO_NUM_14;
// Native 3.3 V (no level-shift); 4.7 kΩ pull-ups per SDA/SCL pair.

// ───────────────────── Analog inputs (ADS1115 channels, A0..A3) ─────────────────────
// ADS1115 powered at 3.3 V (⇒ AIN_max = 3.3 V). Single-ended with respect to GND.
namespace ads
{
constexpr uint8_t VBAT = 0;   // A0: battery voltage via 100k/15k voltage divider (tracked continuously)
// ── Reserved: analog physical joystick (FUTURE, not wired/not used) ──
// Current control is 100% Bluetooth gamepad; we reserve 2 ADS1115 channels to
// later connect an X/Y joystick behind the same `input` abstraction (single-shot reading).
constexpr uint8_t JOY_X = 1;  // A1 — steering (future)
constexpr uint8_t JOY_Y = 2;  // A2 — forward  (future)
// A3: free.
} // namespace ads

// Status outputs
constexpr gpio_num_t LED    = GPIO_NUM_2;    // board LED
constexpr gpio_num_t WS2812 = GPIO_NUM_4;   // status strip

// Active levels
constexpr int BTN_ACTIVE = 0;   // button pressed = low level

// Free GPIOs: 21, 22, 23 and the input-only 34, 35, 36, 39.
// (Quadrature encoders were reserved here as a fallback to the AS5600; dropped — the
// magnetic sensors do the job. See doc/reducteur.md for the kinematics they measure.)

} // namespace pins
