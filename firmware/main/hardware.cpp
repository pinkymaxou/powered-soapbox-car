// hardware.cpp — Hardware access (differential variant: 2 REAR motors, NO sensor at all).
// No inputs of any kind: arming is a gamepad button and power is a physical switch.
// The driver handles (LEDC) live here as statics; the rest of the firmware goes through the
// free functions of the `board` namespace. (The two AS5600 encoders and their I2C buses
// were removed on 2026-09-29, with everything that used them.)
#include "hardware.hpp"

#include <atomic>
#include "soc/dport_reg.h"

#include <cmath>

#include "config.hpp"
#include "pinout.hpp"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char* TAG = "board";

// Rotation direction (level applied to the driver's DIR pin).
enum class Dir : int { Forward = 1, Reverse = 0 };

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

namespace
{
// LEDC timer resolution (esp_driver_ledc type) — the corresponding numeric value
// (PWM_MAX = 4095) lives in control_types.hpp, the only one useful to the logic.
constexpr ledc_timer_bit_t PWM_RES = LEDC_TIMER_12_BIT;


// LEDC clock SENTINEL — diagnostic + auto-repair of the suspected race:
// recurring "Interrupt WDT" crash with the PC frozen on a LEDC register WRITE, under
// radio load (Wi-Fi/BT). Hypothesis: a concurrent read-modify-write (unlocked on the
// radio blob side) on DPORT_PERIP_CLK_EN_REG loses the LEDC clock bit; and writing to a
// peripheral WITHOUT a clock freezes the APB bus on ESP32 → interrupt watchdog with no dump.
// We check the bit BEFORE each motor write: gated off → repaired + counted (System page).
std::atomic<uint32_t> m_ledc_clk_fix{0};

inline void ledcClockGuard()
{
    if (0 == (DPORT_REG_READ(DPORT_PERIP_CLK_EN_REG) & DPORT_LEDC_CLK_EN))
    {
        DPORT_REG_SET_BIT(DPORT_PERIP_CLK_EN_REG, DPORT_LEDC_CLK_EN);
        DPORT_REG_CLR_BIT(DPORT_PERIP_RST_EN_REG, DPORT_LEDC_RST);
        const uint32_t n = m_ledc_clk_fix.fetch_add(1) + 1;
        ESP_LOGE(TAG, "LEDC clock found GATED OFF (DPORT/radio race?) — repaired (n=%lu)",
                 static_cast<unsigned long>(n));
    }
}

// Output state caches: we write LEDC/GPIO ONLY if the value changes. At 500 Hz in
// permanent braking, this eliminates ~2000 useless APB writes/s (and shrinks by the same
// amount the exposure window to the clock race above).
uint32_t m_duty_last[2] = {UINT32_MAX, UINT32_MAX};
int      m_dir_last[2]  = {-1, -1};

void dirPin(gpio_num_t pin, Dir d)
{
    const int idx = (pins::DIR_L == pin) ? 0 : 1;
    if (m_dir_last[idx] == static_cast<int>(d)) return;
    m_dir_last[idx] = static_cast<int>(d);
    gpio_set_level(pin, static_cast<int>(d));
}

void setDuty(ledc_channel_t ch, uint32_t duty)
{
    const int idx = (LEDC_CHANNEL_0 == ch) ? 0 : 1;
    if (m_duty_last[idx] == duty) return;
    ledcClockGuard();
    m_duty_last[idx] = duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

void initLED()
{
    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::LED);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
}

void initMotors()
{
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = PWM_RES;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = hw::PWM_FREQ_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    auto configChannel = [](ledc_channel_t ch, gpio_num_t pin)
    {
        ledc_channel_config_t c{};
        c.gpio_num = pin;
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel = ch;
        c.timer_sel = LEDC_TIMER_0;
        c.duty = 0;
        c.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    };
    configChannel(LEDC_CHANNEL_0, pins::PWM_L);
    configChannel(LEDC_CHANNEL_1, pins::PWM_R);

    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::DIR_L) | (1ULL << pins::DIR_R);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    // The LEDC attach reconfigures the PWM pins: re-arm their internal pull-downs.
    gpio_pulldown_en(pins::PWM_L);
    gpio_pulldown_en(pins::PWM_R);
    dirPin(pins::DIR_L, Dir::Forward);
    dirPin(pins::DIR_R, Dir::Forward);
    board::motorsBrake();   // default state: dynamic braking (never coasting)
}

void motorApply(ledc_channel_t ch, gpio_num_t dir, float v, uint32_t cap)
{
    dirPin(dir, (v >= 0) ? Dir::Forward : Dir::Reverse);
    setDuty(ch, static_cast<uint32_t>(fabsf(clampf(v, -1.f, 1.f)) * cap));
}
} // namespace

// ───────────────────────────── Public API ─────────────────────────────
void board::init()
{
    initLED();
    initMotors();
    board::led(false);
}

void board::led(bool on)
{
    gpio_set_level(pins::LED, on ? 1 : 0);
}

void board::motorsSet(float l, float r, uint32_t cap)
{
    motorApply(LEDC_CHANNEL_0, pins::DIR_L, l, cap);
    motorApply(LEDC_CHANNEL_1, pins::DIR_R, r, cap);
}

void board::motorsBrake()
{
    // Passive dynamic braking: zero duty cycle + DIR low on both channels → both
    // outputs of each bridge are low → motor short-circuited (resists movement).
    setDuty(LEDC_CHANNEL_0, 0);
    setDuty(LEDC_CHANNEL_1, 0);
    dirPin(pins::DIR_L, Dir::Reverse);
    dirPin(pins::DIR_R, Dir::Reverse);
}

uint32_t board::ledcClkFixCount() { return m_ledc_clk_fix.load(); }

void board::motorsIdleEarly()
{
    // PWM/DIR as output at the LOW level from boot, before LEDC init: all low = DYNAMIC
    // BRAKING (both bridge outputs at low level short-circuit the motor). The internal
    // PULL-DOWNs are armed as well: if a pin returns to high impedance while the chip is
    // running, it falls back to the brake side. ⚠️ A RESET clears these pulls
    // (IO_MUX registers): the electrical default during the bootloader depends on the
    // driver's EXTERNAL pull-downs — to be guaranteed on the wiring side (see README).
    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::PWM_L) | (1ULL << pins::DIR_L) |
                      (1ULL << pins::PWM_R) | (1ULL << pins::DIR_R);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    gpio_set_level(pins::PWM_L, 0);
    gpio_set_level(pins::DIR_L, 0);
    gpio_set_level(pins::PWM_R, 0);
    gpio_set_level(pins::DIR_R, 0);
}
