// hardware.cpp — Hardware access (differential variant: 2 front motors + 2 AS5600).
// The driver handles (ADC, LEDC, 2× I2C) live here as statics; the rest of the firmware
// goes through the free functions of the `board` namespace.
#include "hardware.hpp"

#include <atomic>
#include "soc/dport_reg.h"

#include <cmath>

#include "ads1115.hpp"
#include "config.hpp"
#include "pinout.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
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

i2c_master_bus_handle_t   m_bus[2] = {nullptr, nullptr};   // bus 0 = L wheel, bus 1 = R wheel
i2c_master_dev_handle_t   m_as[2]  = {nullptr, nullptr};   // AS5600 per bus
int                       m_angle_last[2] = {-1, -1};      // last raw angle per sensor
Ads1115                   m_ads;                           // external ADC (bus 0), Vbat on A0
// Vbat read tolerance: consecutive FAILED windows before the voltage is declared unknown.
// 10 windows at 20 Hz ≈ 0.5 s of silence — long enough to ride out a bus glitch, short
// enough that a genuinely unplugged sensor is reported before it matters. Recovery is
// immediate: one good read resets the count.
constexpr int VBAT_FAIL_LIMIT = 10;
int                       m_vbat_fails = 0;
long                      m_vbat_acc = 0;
int                       m_vbat_n = 0;
float                     m_vbat_last = -1.f;   // last good reading, held during the tolerance

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

// External ADS1115 ADC (on bus 0). Vbat tracked continuously on A0 at ±4.096 V
// (125 µV resolution): the pin voltage (≤ 3.3 V via the divider) fits with plenty of margin.
void initExtAdc()
{
    if (!m_ads.begin(m_bus[0], hw::ADS1115_ADDR, hw::I2C_FREQ_HZ))
    {
        ESP_LOGW(TAG, "ADS1115 unavailable: Vbat reading at 0");
        return;
    }
    m_ads.startContinuous(pins::ads::VBAT, Ads1115::Gain::FS_4V096, Ads1115::Rate::SPS_128);
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

// Two independent I2C buses, one AS5600 sensor (0x36) per bus.
void initEncoders()
{
    struct { i2c_port_t port; gpio_num_t sda; gpio_num_t scl; } cfg[2] = {
        {I2C_NUM_0, pins::I2C0_SDA, pins::I2C0_SCL},
        {I2C_NUM_1, pins::I2C1_SDA, pins::I2C1_SCL},
    };
    for (int i = 0; i < 2; ++i)
    {
        i2c_master_bus_config_t bus{};
        bus.i2c_port = cfg[i].port;
        bus.sda_io_num = cfg[i].sda;
        bus.scl_io_num = cfg[i].scl;
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = true;
        if (ESP_OK != i2c_new_master_bus(&bus, &m_bus[i]))
        {
            ESP_LOGW(TAG, "I2C bus %d unavailable: no sensor for wheel %c", i, i ? 'D' : 'G');
            continue;
        }
        i2c_device_config_t dev{};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address = hw::AS5600_ADDR;
        dev.scl_speed_hz = hw::I2C_FREQ_HZ;
        if (ESP_OK != i2c_master_bus_add_device(m_bus[i], &dev, &m_as[i]))
        {
            ESP_LOGW(TAG, "AS5600 wheel %c not added", i ? 'D' : 'G');
            m_as[i] = nullptr;
        }
    }
}

// Scans both buses and logs every device that answers. Without this, "ADS1115 absent" is
// indistinguishable from "ADS1115 answering at the wrong address" — the ADDR pin picks
// 0x48..0x4B, and a floating one lands anywhere. Known: 0x36 = AS5600, 0x48..0x4B = ADS1115.
void i2cScan()
{
    for (int b = 0; b < 2; ++b)
    {
        if (!m_bus[b]) continue;
        char found[96] = "";
        size_t n = 0;
        for (uint8_t addr = 0x08; addr < 0x78; ++addr)
        {
            if (ESP_OK != i2c_master_probe(m_bus[b], addr, 5)) continue;
            const char* what = (0x36 == addr) ? " AS5600"
                             : (addr >= 0x48 && addr <= 0x4B) ? " ADS1115" : "";
            n += snprintf(found + n, sizeof(found) - n, " 0x%02X%s", addr, what);
            if (n >= sizeof(found) - 16) break;
        }
        if (0 == n) ESP_LOGW(TAG, "I2C bus %d: NOTHING answers (wiring/pull-ups/power?)", b);
        else        ESP_LOGI(TAG, "I2C bus %d:%s", b, found);
    }
}

// Presence of each AS5600: last I2C read succeeded? (refreshed at 500 Hz by the
// control loop via angleDelta → feeds the "encoder absent" faults per wheel).
std::atomic<bool> m_enc_present[2] = {false, false};

// Magnet-in-field per AS5600 (STATUS register MD/ML/MH), polled at ~10 Hz. Default true so a
// transient/missing read never phantom-faults; a truly absent chip is caught by m_enc_present.
std::atomic<bool> m_mag_ok[2] = {true, true};
int m_mag_tick = 0;

// Raw 12-bit angle (0..4095) of sensor `i`. Returns -1 if absent/error.
// BACK OFF on a sensor that has gone silent. A missing AS5600 costs a full I2C timeout on
// EVERY tick, and measured on the bench that alone stretched the worst 500 Hz tick to 4-9.5 ms
// against a 2 ms budget — the loop running at a fifth of its rate because one wheel was
// unplugged. Once a sensor is known absent we stop asking every tick and probe it at 5 Hz
// instead; the amortised cost collapses and recovery is still automatic and prompt.
// The two sensors retry on OPPOSITE phases so two dead encoders never time out on one tick.
constexpr unsigned ENC_RETRY_TICKS = 100;   // 500 Hz / 100 = 5 Hz while absent
unsigned m_enc_retry[2] = {0, 0};

int readAngleRaw(int i)
{
    if (!m_as[i]) return -1;
    if (!m_enc_present[i].load())
    {
        // Absent: only spend a timeout on the retry tick (phase 0 for the left, 50 for the
        // right), and skip the bus entirely the rest of the time.
        // Phases 7 and 57: 50 ticks apart so the two never collide with each other, and
        // neither is a multiple of VBAT_READ_TICKS (25) so neither lands on a battery read.
        const unsigned phase = m_enc_retry[i]++ % ENC_RETRY_TICKS;
        if (phase != (i ? 57u : 7u)) return -1;
    }
    const uint8_t reg = hw::AS5600_REG_RAWANG;
    uint8_t buf[2] = {0, 0};
    if (ESP_OK != i2c_master_transmit_receive(m_as[i], &reg, 1, buf, 2, hw::I2C_XFER_TIMEOUT_MS))
    {
        m_enc_present[i].store(false);
        m_angle_last[i] = -1;   // the next good read re-anchors instead of inventing a jump
        return -1;
    }
    m_enc_present[i].store(true);
    return ((buf[0] & 0x0F) << 8) | buf[1];
}

// Signed Δangle of sensor `i` since the last call, wrap 0↔4095 → [-2048..2047].
int angleDelta(int i)
{
    const int cur = readAngleRaw(i);
    if (cur < 0) return 0;
    if (m_angle_last[i] < 0)
    {
        m_angle_last[i] = cur;
        return 0;
    }
    int d = cur - m_angle_last[i];
    m_angle_last[i] = cur;
    if (d > 2048)  d -= 4096;
    if (d < -2048) d += 4096;
    return d;
}

void initButton()
{
    // Both inputs idle on the INTERNAL pull-up. That is real on these pins — the sense
    // line once sat on GPIO34, where the pull-up request was silently ignored (GPIO 34-39
    // have no pull hardware) and an unwired pin floated; it lives on GPIO22 now precisely
    // so this line does what it says.
    gpio_config_t in{};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = (1ULL << pins::START_BTN) | (1ULL << pins::MOTOR_PWR_SENSE);
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);
}


// Debounce: a state only changes after BTN_DEBOUNCE_TICKS stable readings.
struct Debounce
{
    bool state = false;
    int  count = 0;
};
Debounce m_db_start;

bool debounce(Debounce& d, bool raw)
{
    if (raw == d.state)
    {
        d.count = 0;
    }
    else if (++d.count >= hw::BTN_DEBOUNCE_TICKS)
    {
        d.state = raw;
        d.count = 0;
    }
    return d.state;
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
    initEncoders();   // 2 I2C buses + 2 AS5600 sensors
    i2cScan();        // log what ACTUALLY answers, before anything is declared absent
    initExtAdc();     // ADS1115 on bus 0 (after the buses are created)
    initButton();
    board::led(false);
}

void board::led(bool on)
{
    gpio_set_level(pins::LED, on ? 1 : 0);
}

// ONE sample per call, averaged over ADC_OVERSAMPLE calls. Called every tick: the 8 reads
// that used to be done back-to-back cost ~3 ms on that single tick — measured, with every
// read SUCCEEDING, so it was never about failures. Spread out, the peak drops to one
// transaction while the averaging and the ~20 Hz output cadence are unchanged.
float board::vbatSample()
{
    if (!m_ads.ok()) return -1.0f;
    int16_t raw = 0;
    if (m_ads.readRaw(raw))
    {
        m_vbat_fails = 0;
        m_vbat_acc += raw;
        if (++m_vbat_n >= hw::ADC_OVERSAMPLE)
        {
            m_vbat_last = m_ads.toVolts(static_cast<int16_t>(m_vbat_acc / m_vbat_n));
            m_vbat_acc = 0;
            m_vbat_n = 0;
        }
        return m_vbat_last;
    }
    // Failed read: hold the last good value, and only admit "unknown" past the threshold.
    if (++m_vbat_fails < VBAT_FAIL_LIMIT) return m_vbat_last;
    if (VBAT_FAIL_LIMIT == m_vbat_fails)
    {
        ESP_LOGW(TAG, "ADS1115 silent for %d reads → Vbat unknown (auto-recovers)", VBAT_FAIL_LIMIT);
    }
    m_vbat_acc = 0;
    m_vbat_n = 0;
    return -1.0f;
}

float board::vbatVolts(int n)
{
    if (!m_ads.ok())
    {
        return -1.0f;   // sensor absent → voltage unknown (the controller skips the LVC)
    }
    // Average of n readings of the conversion register (continuous mode) → voltage at pin A0.
    // BAIL OUT on the first failure instead of hammering the remaining n-1 times: if the chip
    // has gone quiet, each attempt costs a full I2C timeout, and this runs inside the 500 Hz
    // control loop. One failed read is enough to know this window is a write-off.
    long acc = 0;
    int  got = 0;
    for (int k = 0; k < n; ++k)
    {
        int16_t raw = 0;
        if (!m_ads.readRaw(raw)) break;
        acc += raw;
        ++got;
    }
    if (got > 0)
    {
        m_vbat_fails = 0;                     // any good read clears the count immediately
        m_vbat_last = m_ads.toVolts(static_cast<int16_t>(acc / got));
        return m_vbat_last;
    }
    // The window failed. Do NOT cry wolf on one glitch — a single dropped transaction on a
    // shared bus is normal. Hold the last good value and keep counting; only past the
    // threshold (~0.5 s at 20 Hz) do we admit the voltage is unknown.
    // Returning 0.0 here used to be a real bug: 0 is a perfectly valid voltage to the caller,
    // so vbat_ok stayed true, NO_VBAT was never raised, and the LVC read "0 V" — below every
    // cutoff — disarmed the kart and cut the power 30 s later, blaming a flat battery.
    if (++m_vbat_fails < VBAT_FAIL_LIMIT)
    {
        return m_vbat_last;
    }
    if (VBAT_FAIL_LIMIT == m_vbat_fails)
    {
        ESP_LOGW(TAG, "ADS1115 silent for %d reads → Vbat unknown (auto-recovers)", VBAT_FAIL_LIMIT);
    }
    return -1.0f;
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

int board::encLeftDelta()  { return angleDelta(0); }
int board::encRightDelta() { return angleDelta(1); }
bool board::encLeftPresent()  { return m_enc_present[0].load(); }
bool board::encRightPresent() { return m_enc_present[1].load(); }
bool board::encLeftMagOk()  { return m_mag_ok[0].load(); }
bool board::encRightMagOk() { return m_mag_ok[1].load(); }

// Poll the AS5600 STATUS register (0x0B) for both buses at ~10 Hz (MAG_READ_TICKS). Magnet OK
// = MD set AND not too weak (ML) AND not too strong (MH). On read failure, keep the last value.
// STAGGERED, one bus per poll, and deliberately off the ticks where Vbat is read. Every I2C
// call here can block for a full timeout if its sensor has gone quiet, so what matters is not
// the average cost but how many of them can pile onto the SAME tick. Reading both buses
// together, on a tick that was also a Vbat tick, put 5 potentially-timing-out transactions on
// one tick; one bus at a time, phase-shifted, caps it at 3.
void board::refreshMagStatus()
{
    const unsigned tick = m_mag_tick++;
    // Phase 13 and 38: never a multiple of VBAT_READ_TICKS (25), so a magnet poll and a
    // battery read can never land on the same tick.
    const unsigned phase = tick % hw::MAG_READ_TICKS;
    int i;
    if (13 == phase)      i = 0;
    else if (38 == phase) i = 1;
    else                  return;

    if (!m_as[i]) { m_mag_ok[i].store(false); return; }
    if (!m_enc_present[i].load()) return;   // already known absent: do not pay a second timeout
    const uint8_t reg = hw::AS5600_REG_STATUS;
    uint8_t s = 0;
    if (ESP_OK == i2c_master_transmit_receive(m_as[i], &reg, 1, &s, 1, hw::I2C_XFER_TIMEOUT_MS))
        m_mag_ok[i].store((s & hw::AS5600_MD) && !(s & hw::AS5600_ML) && !(s & hw::AS5600_MH));
}

void board::pollButtons()
{
    debounce(m_db_start, pins::BTN_ACTIVE == gpio_get_level(pins::START_BTN));
}

bool board::btnStart()
{
    return m_db_start.state;
}

// 40 A relay COIL energized (= e-stop released) = opto conducting = pin pulled LOW. An
// unwired or broken input rises to the internal pull-up and reads "engaged", the safe way
// round. RAW level: the debounce lives in the controller core (hw::PWR_SENSE_DEBOUNCE_TICKS),
// where the sim can test it. ALWAYS consulted (no software bypass): bench without the
// opto = GPIO22 tied to GND.
bool board::motorPowerLive()
{
    return 0 == gpio_get_level(pins::MOTOR_PWR_SENSE);
}

void board::powerLatch()
{
    // Active LOW: pull the opto's LED to ground → the opto sends +20 V to the gate.
    gpio_set_level(pins::POWER_HOLD, 0);
    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::POWER_HOLD);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;   // active LOW: even in high impedance, stay "held"
    gpio_config(&io);
    gpio_set_level(pins::POWER_HOLD, 0);
}

void board::powerOff()
{
    gpio_set_level(pins::POWER_HOLD, 1);
}

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
