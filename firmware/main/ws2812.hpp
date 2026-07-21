// ws2812.hpp — Minimal driver for the addressable WS2812B LED strip (via RMT, no dependency).
#pragma once

#include <cstdint>
#include <vector>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"

class Ws2812
{
public:
    // Initializes the strip on `pin` with `count` LEDs. brightness ∈ [0..255].
    void init(gpio_num_t pin, int count, uint8_t brightness = 64);
    void setBrightness(uint8_t b)
    {
        m_brightness = b;
    }
    void setAll(uint8_t r, uint8_t g, uint8_t b);
    void setPixel(int i, uint8_t r, uint8_t g, uint8_t b);
    void show();                            // pushes the buffer to the strip
    int count() const
    {
        return m_count;
    }

private:
    rmt_channel_handle_t m_chan = nullptr;
    rmt_encoder_handle_t m_enc = nullptr;
    std::vector<uint8_t> m_buf;             // GRB bytes (3 per LED)
    int                  m_count = 0;
    uint8_t              m_brightness = 64;
};
