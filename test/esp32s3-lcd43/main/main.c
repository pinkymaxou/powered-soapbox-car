// Firmware de TEST — ESP32-S3 + écran tactile RGB 4,3" (800×480).
// Affiche un CARRÉ ROUGE en bas-gauche (test dalle) + une HORLOGE (NTP) en haut-droite.
//
// Chaîne : CH422G (I²C) allume le rétroéclairage + relâche les resets → panneau RGB
// (esp_lcd) → LVGL (esp_lvgl_port). Wi-Fi + NTP via wifi_web. Brochage : voir doc/HARDWARE.md.
#include <string.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "periph.h"
#include "wifi_web.h"

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

// Pixel clock — levier principal du taux de rafraîchissement. On le pousse au max stable.
// refresh ≈ pclk / ((H+hpw+hbp+hfp) · (V+vpw+vbp+vfp)). Blanking serré = plus de Hz.
// 16 MHz = valeur éprouvée pour cette dalle (configs officielles/communautaires). Au-delà,
// la bande passante PSRAM 80 MHz ne suit pas le DMA RGB → underruns / glitchs / image qui saute.
#define LCD_PCLK_HZ (16 * 1000 * 1000)
#define H_PW 4
#define H_BP 8
#define H_FP 8
#define V_PW 4
#define V_BP 8
#define V_FP 8

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
    // EXIO HAUT sauf EXIO4 (= SD_CS/enable, actif BAS) ; EXIO5 HAUT = mode CAN.
    // bit2 = rétroéclairage ON, bit4 = 0 (SD activée), bit5 = 1 (transceiver en mode CAN).
    const uint8_t io = 0xEF;
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
        .bounce_buffer_size_px = LCD_H_RES * 10,      // requis ici pour une image stable (anti-roll)
        .dma_burst_size = 64,
        .de_gpio_num = PIN_RGB_DE,
        .pclk_gpio_num = PIN_RGB_PCLK,
        .vsync_gpio_num = PIN_RGB_VSYNC,
        .hsync_gpio_num = PIN_RGB_HSYNC,
        .disp_gpio_num = -1,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = H_PW,
            .hsync_back_porch = H_BP,
            .hsync_front_porch = H_FP,
            .vsync_pulse_width = V_PW,
            .vsync_back_porch = V_BP,
            .vsync_front_porch = V_FP,
            .flags.pclk_active_neg = true,
        },
        .flags.fb_in_psram = true,
    };
    for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = RGB_DATA_GPIO[i];

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    const int h_total = LCD_H_RES + H_PW + H_BP + H_FP;
    const int v_total = LCD_V_RES + V_PW + V_BP + V_FP;
    const float refresh = (float)LCD_PCLK_HZ / (h_total * v_total);
    ESP_LOGI(TAG, "Panneau RGB %dx%d — pclk %.1f MHz → refresh ≈ %.1f Hz",
             LCD_H_RES, LCD_V_RES, LCD_PCLK_HZ / 1e6f, refresh);
    return panel;
}

static lv_obj_t* s_clock = NULL;
static void clock_timer_cb(lv_timer_t* t);

// Carré rouge en bas-gauche + horloge en haut-droite (fond noir).
static void ui_build(void)
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

    s_clock = lv_label_create(scr);
    lv_obj_set_style_text_color(s_clock, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_clock, "--:--:--");
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_timer_create(clock_timer_cb, 1000, NULL);   // mise à jour dans la tâche LVGL
    lvgl_port_unlock();
}

// Met à jour l'horloge chaque seconde. Appelée par un lv_timer → s'exécute DANS la tâche de
// rendu LVGL (pas de tâche concurrente ni de verrou → évite toute « bataille » de redessin).
static void clock_timer_cb(lv_timer_t* t)
{
    (void)t;
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[16];
    if (tm.tm_year > (2020 - 1900)) strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    else strcpy(buf, "--:--:--");
    lv_label_set_text(s_clock, buf);
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
            .direct_mode = true,   // ne redessine que les zones modifiées (l'horloge) — plus léger que full_refresh
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,         // bounce buffer (requis ici pour une image stable)
            .avoid_tearing = true,   // anti-déchirure par bascule des 2 framebuffers
        },
    };
    lv_display_t* disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (NULL == disp)
    {
        ESP_LOGE(TAG, "Échec de l'ajout de l'écran LVGL");
        return;
    }

    ui_build();
    ESP_LOGI(TAG, "Carré rouge + horloge affichés. Test écran prêt.");

    // Wi-Fi (AP+STA) + page web de configuration + NTP (power-save OFF pour limiter les glitchs).
    wifi_web_start();

    // Périphériques de test : carte SD + bus CAN.
    sd_init();
    can_init();
}
