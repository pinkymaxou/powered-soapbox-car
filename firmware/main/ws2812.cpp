// ws2812.cpp — WS2812B driver via the RMT peripheral (bytes encoder).
#include "ws2812.hpp"

#include "driver/rmt_encoder.h"
#include "esp_log.h"

static const char* TAG = "ws2812";
static constexpr uint32_t RES_HZ = 10'000'000;  // 0.1 µs / tick

void Ws2812::init(gpio_num_t pin, int count, uint8_t brightness)
{
    m_count = count;
    m_brightness = brightness;
    m_buf.assign(m_count * 3, 0);

    rmt_tx_channel_config_t chan_cfg{};
    chan_cfg.gpio_num = pin;
    chan_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    chan_cfg.resolution_hz = RES_HZ;
    chan_cfg.mem_block_symbols = 64;
    chan_cfg.trans_queue_depth = 4;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &m_chan));

    // Bytes encoder: "bit 0" and "bit 1" symbols at the WS2812B timing.
    rmt_bytes_encoder_config_t enc_cfg{};
    enc_cfg.bit0.level0 = 1; enc_cfg.bit0.duration0 = 3;   // 0.3 µs high
    enc_cfg.bit0.level1 = 0; enc_cfg.bit0.duration1 = 9;   // 0.9 µs low
    enc_cfg.bit1.level0 = 1; enc_cfg.bit1.duration0 = 9;   // 0.9 µs high
    enc_cfg.bit1.level1 = 0; enc_cfg.bit1.duration1 = 3;   // 0.3 µs low
    enc_cfg.flags.msb_first = 1;
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &m_enc));

    ESP_ERROR_CHECK(rmt_enable(m_chan));
    show();  // off at startup
    ESP_LOGI(TAG, "WS2812B strip: %d LEDs on GPIO%d", m_count, static_cast<int>(pin));
}

void Ws2812::setPixel(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (i < 0 || i >= m_count)
    {
        return;
    }
    // Scaling by brightness, GRB order.
    m_buf[i * 3 + 0] = static_cast<uint16_t>(g) * m_brightness / 255;
    m_buf[i * 3 + 1] = static_cast<uint16_t>(r) * m_brightness / 255;
    m_buf[i * 3 + 2] = static_cast<uint16_t>(b) * m_brightness / 255;
}

void Ws2812::setAll(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < m_count; ++i)
    {
        setPixel(i, r, g, b);
    }
}

void Ws2812::show()
{
    if (nullptr == m_chan || nullptr == m_enc || m_buf.empty())
    {
        return;
    }
    rmt_transmit_config_t tx{};
    tx.loop_count = 0;
    rmt_transmit(m_chan, m_enc, m_buf.data(), m_buf.size(), &tx);
    rmt_tx_wait_all_done(m_chan, 100);
}
