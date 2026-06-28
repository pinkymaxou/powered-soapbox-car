// ads1115.hpp — Driver pour le convertisseur A/N externe ADS1115 (16 bits, I2C, PGA).
// Remplace l'ADC interne de l'ESP32 (12 bits, non linéaire, ADC2 en conflit avec le Wi-Fi)
// par une mesure plus précise et stable. Se branche en piggyback sur un bus I2C existant.
//
// 4 entrées single-ended (A0..A3), adresse I2C réglable par la broche ADDR :
//   ADDR→GND = 0x48 · →VDD = 0x49 · →SDA = 0x4A · →SCL = 0x4B.
// ⚠️ Alimenter en 3,3 V (niveaux I2C compatibles ESP32) ; AIN_max = VDD = 3,3 V.
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

class Ads1115
{
public:
    // Pleine échelle du PGA (±V). Le gain conditionne la résolution (LSB = FS / 32768).
    enum class Gain : uint8_t
    {
        FS_6V144 = 0,
        FS_4V096 = 1,
        FS_2V048 = 2,   // défaut puce
        FS_1V024 = 3,
        FS_0V512 = 4,
        FS_0V256 = 5,
    };

    // Cadence de conversion (échantillons/s). Plus rapide = plus de bruit.
    enum class Rate : uint8_t
    {
        SPS_8 = 0,
        SPS_16,
        SPS_32,
        SPS_64,
        SPS_128,   // défaut puce
        SPS_250,
        SPS_475,
        SPS_860,
    };

    // Attache l'ADS1115 (déjà câblé) à un bus I2C existant. false s'il est absent.
    bool begin(i2c_master_bus_handle_t bus, uint8_t addr = 0x48, uint32_t scl_hz = 400000);

    bool ok() const { return nullptr != m_dev; }

    // Démarre une conversion CONTINUE sur un canal single-ended (0..3) : les lectures
    // suivantes (readRaw) ne sont qu'un accès registre rapide → idéal pour une voie suivie
    // en boucle (ex. tension batterie). Mémorise gain/cadence courants.
    bool startContinuous(uint8_t channel, Gain gain, Rate rate);

    // Lecture UNIQUE (single-shot) d'un canal single-ended (0..3) : déclenche une conversion
    // puis attend son achèvement (≈ 1/cadence). Pratique pour scruter plusieurs voies.
    bool readSingleShot(uint8_t channel, Gain gain, Rate rate, int16_t& raw);

    // Dernière conversion du mode continu (lit le registre de conversion).
    bool readRaw(int16_t& raw);

    // Conversion brut → volts selon le gain mémorisé.
    float toVolts(int16_t raw) const { return raw * lsbVolts(m_gain); }

    // Volts par LSB pour un gain donné (FS / 32768).
    static float lsbVolts(Gain gain);

private:
    uint16_t buildConfig(uint8_t channel, Gain gain, Rate rate, bool continuous) const;
    bool     writeReg(uint8_t reg, uint16_t value);
    bool     readReg(uint8_t reg, uint16_t& value);

    i2c_master_dev_handle_t m_dev  = nullptr;
    Gain                    m_gain = Gain::FS_2V048;
    Rate                    m_rate = Rate::SPS_128;
};
