// ads1115.cpp — ADS1115 driver implementation (see ads1115.hpp).
#include "ads1115.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ads1115";

namespace
{
// Register pointers (1st byte of each transaction).
constexpr uint8_t REG_CONVERSION = 0x00;
constexpr uint8_t REG_CONFIG     = 0x01;

// Configuration register fields (16 bits).
constexpr uint16_t CFG_OS_SINGLE   = 0x8000;  // write 1 = starts a conversion; read 1 = idle
constexpr uint16_t CFG_MUX_SE_BASE = 0x4000;  // MUX = 100b | channel → A0..A3 vs GND (bits 14..12)
constexpr uint16_t CFG_MODE_SINGLE = 0x0100;  // 1 = single-shot, 0 = continuous (bit 8)
constexpr uint16_t CFG_COMP_OFF    = 0x0003;  // COMP_QUE = 11b: comparator disabled (bits 1..0)

constexpr int CONV_TIMEOUT_MS = 20;   // margin beyond the slowest conversion time

// Approximate conversion time (ms) per rate, for the single-shot timing.
int convMs(Ads1115::Rate rate)
{
    constexpr int MS[] = {125, 63, 32, 16, 8, 4, 3, 2};  // 8..860 SPS
    const int idx = static_cast<int>(rate);
    return (idx >= 0 && idx < 8) ? MS[idx] : 8;
}
} // namespace

float Ads1115::lsbVolts(Gain gain)
{
    // Full scale (±V) according to the gain, then FS / 32768 (15 bits on the positive side).
    constexpr float FS[] = {6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f};
    const int idx = static_cast<int>(gain);
    const float fs = (idx >= 0 && idx < 6) ? FS[idx] : 2.048f;
    return fs / 32768.0f;
}

uint16_t Ads1115::buildConfig(uint8_t channel, Gain gain, Rate rate, bool continuous) const
{
    uint16_t cfg = CFG_OS_SINGLE;                                 // OS = 1
    cfg |= CFG_MUX_SE_BASE | (static_cast<uint16_t>(channel & 0x03) << 12);  // MUX single-ended
    cfg |= static_cast<uint16_t>(gain) << 9;                      // PGA
    if (!continuous) cfg |= CFG_MODE_SINGLE;                      // MODE
    cfg |= static_cast<uint16_t>(rate) << 5;                      // DR
    cfg |= CFG_COMP_OFF;                                          // comparator off
    return cfg;
}

bool Ads1115::writeReg(uint8_t reg, uint16_t value)
{
    const uint8_t buf[3] = {reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)};
    return ESP_OK == i2c_master_transmit(m_dev, buf, sizeof(buf), CONV_TIMEOUT_MS);
}

bool Ads1115::readReg(uint8_t reg, uint16_t& value)
{
    uint8_t rx[2] = {0, 0};
    if (ESP_OK != i2c_master_transmit_receive(m_dev, &reg, 1, rx, 2, CONV_TIMEOUT_MS))
    {
        return false;
    }
    value = (static_cast<uint16_t>(rx[0]) << 8) | rx[1];
    return true;
}

bool Ads1115::begin(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t scl_hz)
{
    if (nullptr == bus)
    {
        return false;
    }
    if (ESP_OK != i2c_master_probe(bus, addr, CONV_TIMEOUT_MS))
    {
        ESP_LOGW(TAG, "ADS1115 absent at address 0x%02X", addr);
        return false;
    }
    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = addr;
    dev.scl_speed_hz    = scl_hz;
    if (ESP_OK != i2c_master_bus_add_device(bus, &dev, &m_dev))
    {
        m_dev = nullptr;
        ESP_LOGW(TAG, "Adding the ADS1115 device failed (0x%02X)", addr);
        return false;
    }
    ESP_LOGI(TAG, "ADS1115 detected at 0x%02X", addr);
    return true;
}

bool Ads1115::startContinuous(uint8_t channel, Gain gain, Rate rate)
{
    if (!ok())
    {
        return false;
    }
    m_gain = gain;
    m_rate = rate;
    return writeReg(REG_CONFIG, buildConfig(channel, gain, rate, /*continuous=*/true));
}

bool Ads1115::readRaw(int16_t& raw)
{
    uint16_t v = 0;
    if (!ok() || !readReg(REG_CONVERSION, v))
    {
        return false;
    }
    raw = static_cast<int16_t>(v);
    return true;
}

bool Ads1115::readSingleShot(uint8_t channel, Gain gain, Rate rate, int16_t& raw)
{
    if (!ok())
    {
        return false;
    }
    m_gain = gain;
    m_rate = rate;
    if (!writeReg(REG_CONFIG, buildConfig(channel, gain, rate, /*continuous=*/false)))
    {
        return false;
    }
    // Wait for the conversion to finish: poll the OS bit (=1 when done), bounded by the
    // theoretical conversion time + a margin.
    const int deadline = convMs(rate) + 2;
    for (int waited = 0; waited <= deadline; ++waited)
    {
        vTaskDelay(pdMS_TO_TICKS(1));
        uint16_t cfg = 0;
        if (readReg(REG_CONFIG, cfg) && (cfg & CFG_OS_SINGLE))
        {
            break;
        }
    }
    return readRaw(raw);
}
