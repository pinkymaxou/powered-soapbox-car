// hardware.cpp — Implémentation de l'accès matériel.
// Les instances/handles des drivers (ADC, LEDC, I2C) vivent ici en statique ;
// le reste du firmware passe par les fonctions libres du namespace `board`.
#include "hardware.hpp"

#include <cmath>

#include "config.hpp"
#include "pinout.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char* TAG = "board";

// Sens de rotation (niveau appliqué sur la broche DIR du driver).
enum class Dir : int { Forward = 1, Reverse = 0 };

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

namespace
{
// État matériel (instances des drivers)
adc_oneshot_unit_handle_t m_adc = nullptr;
adc_cali_handle_t         m_cali = nullptr;
i2c_master_bus_handle_t   m_i2c_bus = nullptr;
i2c_master_dev_handle_t   m_as5600 = nullptr;
int                       m_angle_last = -1;   // dernier angle brut AS5600 (-1 = non initialisé)
bool                      m_led_on = false;

void dirPin(gpio_num_t pin, Dir d)
{
    gpio_set_level(pin, static_cast<int>(d));
}

void setDuty(ledc_channel_t ch, uint32_t duty)
{
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

void initADC()
{
    adc_oneshot_unit_init_cfg_t init_cfg{};
    init_cfg.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &m_adc));

    adc_oneshot_chan_cfg_t chan_cfg{};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(m_adc, pins::THROTTLE, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(m_adc, pins::VBAT, &chan_cfg));

    adc_cali_line_fitting_config_t cali_cfg{};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (ESP_OK != adc_cali_create_scheme_line_fitting(&cali_cfg, &m_cali))
    {
        m_cali = nullptr;
        ESP_LOGW(TAG, "Calibration ADC indisponible (tension approximative)");
    }
}

void initMotors()
{
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = hw::PWM_RES;
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
    gpio_config(&io);
    dirPin(pins::DIR_L, Dir::Forward);
    dirPin(pins::DIR_R, Dir::Forward);
    board::motorsStop();
}

void initEncoder()
{
    // Bus I2C maître sur SDA/SCL (pull-ups internes activés en plus des 4,7 kΩ externes).
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = pins::I2C_SDA;
    bus.scl_io_num = pins::I2C_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    if (ESP_OK != i2c_new_master_bus(&bus, &m_i2c_bus))
    {
        ESP_LOGW(TAG, "Bus I2C indisponible : pas de capteur de vitesse");
        return;
    }
    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = hw::AS5600_ADDR;
    dev.scl_speed_hz = hw::I2C_FREQ_HZ;
    if (ESP_OK != i2c_master_bus_add_device(m_i2c_bus, &dev, &m_as5600))
    {
        ESP_LOGW(TAG, "AS5600 non détecté (0x%02X)", hw::AS5600_ADDR);
        m_as5600 = nullptr;
    }
}

// Lit l'angle brut 12 bits (0..4095). Retourne -1 sur erreur / capteur absent.
int readAngleRaw()
{
    if (!m_as5600) return -1;
    uint8_t reg = hw::AS5600_REG_RAWANG;
    uint8_t buf[2] = {0, 0};
    if (ESP_OK != i2c_master_transmit_receive(m_as5600, &reg, 1, buf, 2, 20))
    {
        return -1;
    }
    return ((buf[0] & 0x0F) << 8) | buf[1];
}

void initButtonsOutputs()
{
    gpio_config_t in{};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = (1ULL << pins::START_BTN) | (1ULL << pins::REVERSE_BTN);
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);

    gpio_config_t out{};
    out.mode = GPIO_MODE_OUTPUT;
    out.pin_bit_mask = (1ULL << pins::REVERSE_LED);
    gpio_config(&out);
    gpio_set_level(pins::REVERSE_LED, 0);
}

// Anti-rebond : un état ne change qu'après BTN_DEBOUNCE_TICKS lectures stables.
struct Debounce
{
    bool state = false;
    int  count = 0;
};
Debounce m_db_start;
Debounce m_db_rev;

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

int adcRawAvg(adc_channel_t ch, int n)
{
    long acc = 0;
    for (int k = 0; k < n; ++k)
    {
        int raw = 0;
        adc_oneshot_read(m_adc, ch, &raw);
        acc += raw;
    }
    return n ? static_cast<int>(acc / n) : 0;
}

// Δangle signé depuis le dernier appel, avec gestion du wrap 0↔4095 → [-2048..2047].
// Le signe donne le sens de rotation. Retourne 0 si capteur absent (pas de fausse vitesse).
int angleDelta()
{
    int cur = readAngleRaw();
    if (cur < 0) return 0;
    if (m_angle_last < 0) { m_angle_last = cur; return 0; }
    int d = cur - m_angle_last;
    m_angle_last = cur;
    if (d > 2048)  d -= 4096;
    if (d < -2048) d += 4096;
    return d;
}

void motorApply(ledc_channel_t ch, gpio_num_t dir, float v, uint32_t cap)
{
    dirPin(dir, (v >= 0) ? Dir::Forward : Dir::Reverse);
    setDuty(ch, static_cast<uint32_t>(fabsf(clampf(v, -1.f, 1.f)) * cap));
}
} // namespace

// ───────────────────────────── API publique ─────────────────────────────
void board::init()
{
    initLED();
    initADC();
    initMotors();
    initEncoder();   // bus I2C + capteur d'angle AS5600
    initButtonsOutputs();
    board::led(false);
}

void board::led(bool on)
{
    gpio_set_level(pins::LED, on ? 1 : 0);
    m_led_on = on;
}

void board::ledToggle()
{
    board::led(!m_led_on);
}

int board::throttleRaw(int n)
{
    return adcRawAvg(pins::THROTTLE, n);
}

float board::vbatVolts(int n)
{
    int raw = adcRawAvg(pins::VBAT, n);
    int mv = 0;
    if (m_cali && ESP_OK == adc_cali_raw_to_voltage(m_cali, raw, &mv))
    {
        return mv / 1000.0f;
    }
    return (raw / 4095.0f) * 3.3f;
}

void board::motorsSet(float l, float r, uint32_t cap)
{
    motorApply(LEDC_CHANNEL_0, pins::DIR_L, l, cap);
    motorApply(LEDC_CHANNEL_1, pins::DIR_R, r, cap);
}

void board::motorsBrake(float strength, uint32_t cap)
{
    uint32_t duty = static_cast<uint32_t>(clampf(strength, 0.f, 1.f) * cap);
    dirPin(pins::DIR_L, Dir::Reverse);
    dirPin(pins::DIR_R, Dir::Reverse);
    setDuty(LEDC_CHANNEL_0, duty);
    setDuty(LEDC_CHANNEL_1, duty);
}

void board::motorsStop()
{
    setDuty(LEDC_CHANNEL_0, 0);
    setDuty(LEDC_CHANNEL_1, 0);
}

int board::encLeftDelta()
{
    return angleDelta();   // Δcounts AS5600 (signé) depuis le dernier appel
}

int board::encRightDelta()
{
    return 0;   // réserve : 2e capteur = 2e bus I2C (adresse AS5600 fixe), non câblé
}

void board::pollButtons()
{
    debounce(m_db_start, pins::BTN_ACTIVE == gpio_get_level(pins::START_BTN));
    debounce(m_db_rev,   pins::BTN_ACTIVE == gpio_get_level(pins::REVERSE_BTN));
}

bool board::btnStart()   { return m_db_start.state; }
bool board::btnReverse() { return m_db_rev.state; }

void board::reverseLED(bool on)
{
    gpio_set_level(pins::REVERSE_LED, on ? 1 : 0);
}

void board::powerLatch()
{
    // Actif BAS : tirer la LED de l'opto vers la masse → l'opto envoie le +20 V sur la gate.
    gpio_set_level(pins::POWER_HOLD, 0);   // mettre l'état AVANT de configurer en sortie
    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::POWER_HOLD);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(pins::POWER_HOLD, 0);   // maintient l'alimentation (sink opto)
}

void board::powerOff()
{
    gpio_set_level(pins::POWER_HOLD, 1);   // relâche la LED de l'opto → coupure
}
