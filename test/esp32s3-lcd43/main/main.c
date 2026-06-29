// Firmware de TEST — ESP32-S3 + écran tactile RGB 4,3" (800×480).
// Objectif : afficher un CARRÉ ROUGE en bas-gauche via LVGL pour valider la dalle.
//
// Chaîne : CH422G (I²C) allume le rétroéclairage + relâche les resets → panneau RGB
// (esp_lcd) → LVGL (esp_lvgl_port). Brochage : voir doc/HARDWARE.md.
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char* TAG = "lcd43";

// ───────────────────────── Brochage (doc/HARDWARE.md) ─────────────────────────
#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9
#define PIN_RGB_DE 5
#define PIN_RGB_VSYNC 3
#define PIN_RGB_HSYNC 46
#define PIN_RGB_PCLK 7
static const int RGB_DATA_GPIO[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};

#define LCD_H_RES 800
#define LCD_V_RES 480

// CH422G : adresses « commande » I²C (interface multi-adresses).
#define CH422G_WR_SET 0x24   // registre de configuration système
#define CH422G_WR_IO 0x38    // niveaux de sortie EXIO0..7

// Allume le rétroéclairage et relâche les resets via le CH422G.
// EXIO actifs : reset tactile, reset LCD, activation rétroéclairage, CS SD — tous HAUT.
static void ch422g_enable_display(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t devc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev_set = NULL, dev_io = NULL;
    devc.device_address = CH422G_WR_SET;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &devc, &dev_set));
    devc.device_address = CH422G_WR_IO;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &devc, &dev_io));

    const uint8_t set = 0x01;   // IO0..7 en sortie push-pull
    const uint8_t io = 0xFF;    // toutes les EXIO HAUT (rétroéclairage ON, resets relâchés)
    ESP_ERROR_CHECK(i2c_master_transmit(dev_set, &set, 1, 100));
    ESP_ERROR_CHECK(i2c_master_transmit(dev_io, &io, 1, 100));
    ESP_LOGI(TAG, "CH422G : rétroéclairage activé, resets relâchés");
}

static esp_lcd_panel_handle_t rgb_panel_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        .num_fbs = 2,                                 // double framebuffer en PSRAM (anti-déchirure)
        .bounce_buffer_size_px = LCD_H_RES * 10,      // accélère le DMA depuis la PSRAM
        .dma_burst_size = 64,
        .de_gpio_num = PIN_RGB_DE,
        .pclk_gpio_num = PIN_RGB_PCLK,
        .vsync_gpio_num = PIN_RGB_VSYNC,
        .hsync_gpio_num = PIN_RGB_HSYNC,
        .disp_gpio_num = -1,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = true,
        },
        .flags.fb_in_psram = true,
    };
    for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = RGB_DATA_GPIO[i];

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_LOGI(TAG, "Panneau RGB %dx%d initialisé", LCD_H_RES, LCD_V_RES);
    return panel;
}

// Carré rouge en bas-gauche sur fond noir.
static void ui_red_square(void)
{
    lvgl_port_lock(0);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t* sq = lv_obj_create(scr);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, 120, 120);
    lv_obj_align(sq, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(sq, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lvgl_port_unlock();
}

void app_main(void)
{
    // Bus I²C partagé (CH422G + tactile).
    i2c_master_bus_config_t busc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&busc, &i2c_bus));

    ch422g_enable_display(i2c_bus);
    esp_lcd_panel_handle_t panel = rgb_panel_init();

    // LVGL (esp_lvgl_port) — tâche LVGL gérée par le port.
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
            .full_refresh = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };
    lv_display_t* disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (NULL == disp)
    {
        ESP_LOGE(TAG, "Échec de l'ajout de l'écran LVGL");
        return;
    }

    ui_red_square();
    ESP_LOGI(TAG, "Carré rouge affiché (bas-gauche). Test écran prêt.");
}
