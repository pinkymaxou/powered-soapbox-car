// ads1115.hpp — Driver for the external ADS1115 A/D converter (16 bits, I2C, PGA).
// Replaces the ESP32's internal ADC (12 bits, nonlinear, ADC2 conflicting with Wi-Fi)
// with a more precise and stable measurement. Connects piggyback onto an existing I2C bus.
//
// 4 single-ended inputs (A0..A3), I2C address set by the ADDR pin:
//   ADDR→GND = 0x48 · →VDD = 0x49 · →SDA = 0x4A · →SCL = 0x4B.
// ⚠️ Power at 3.3 V (ESP32-compatible I2C levels); AIN_max = VDD = 3.3 V.
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

class Ads1115
{
public:
    // PGA full scale (±V). The gain determines the resolution (LSB = FS / 32768).
    enum class Gain : uint8_t
    {
        FS_6V144 = 0,
        FS_4V096 = 1,
        FS_2V048 = 2,   // chip default
        FS_1V024 = 3,
        FS_0V512 = 4,
        FS_0V256 = 5,
    };

    // Conversion rate (samples/s). Faster = more noise.
    enum class Rate : uint8_t
    {
        SPS_8 = 0,
        SPS_16,
        SPS_32,
        SPS_64,
        SPS_128,   // chip default
        SPS_250,
        SPS_475,
        SPS_860,
    };

    // Attaches the ADS1115 (already wired) to an existing I2C bus. false if it is absent.
    bool begin(i2c_master_bus_handle_t bus, uint8_t addr = 0x48, uint32_t scl_hz = 400000);

    bool ok() const { return nullptr != m_dev; }

    // Starts a CONTINUOUS conversion on a single-ended channel (0..3): the subsequent
    // readings (readRaw) are just a fast register access → ideal for a channel tracked
    // in a loop (e.g. battery voltage). Stores the current gain/rate.
    bool startContinuous(uint8_t channel, Gain gain, Rate rate);

    // SINGLE reading (single-shot) of a single-ended channel (0..3): triggers a conversion
    // then waits for its completion (≈ 1/rate). Handy for polling several channels.
    bool readSingleShot(uint8_t channel, Gain gain, Rate rate, int16_t& raw);

    // Last conversion of the continuous mode (reads the conversion register).
    bool readRaw(int16_t& raw);

    // Conversion raw → volts according to the stored gain.
    float toVolts(int16_t raw) const { return raw * lsbVolts(m_gain); }

    // Volts per LSB for a given gain (FS / 32768).
    static float lsbVolts(Gain gain);

private:
    uint16_t buildConfig(uint8_t channel, Gain gain, Rate rate, bool continuous) const;
    bool     writeReg(uint8_t reg, uint16_t value);
    bool     readReg(uint8_t reg, uint16_t& value);

    i2c_master_dev_handle_t m_dev  = nullptr;
    Gain                    m_gain = Gain::FS_2V048;
    Rate                    m_rate = Rate::SPS_128;
};
