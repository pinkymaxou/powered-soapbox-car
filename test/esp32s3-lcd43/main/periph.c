// periph.c — Carte SD (SDMMC 1-bit) + bus CAN (TWAI). Voir doc/HARDWARE.md.
#include "periph.h"

#include "driver/sdmmc_host.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char* TAG = "periph";

// ───────────────────────── Carte SD (SDMMC 1 bit) ─────────────────────────
// MOSI/CMD = GPIO11, SCK/CLK = GPIO12, MISO/D0 = GPIO13 (pas de CS en mode SDMMC).
#define SD_CLK 12
#define SD_CMD 11
#define SD_D0 13

void sd_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t* card = NULL;
    esp_err_t r = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mnt, &card);
    if (ESP_OK != r)
    {
        ESP_LOGW(TAG, "Carte SD non montée (%s) — insérée ?", esp_err_to_name(r));
        return;
    }
    const uint64_t mb = ((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024);
    ESP_LOGI(TAG, "Carte SD montée sur /sdcard : %llu Mo", mb);
}

// ───────────────────────── Bus CAN (TWAI) ─────────────────────────
// TX = GPIO15, RX = GPIO16, 1 Mbit/s. Transceiver en mode CAN via EXIO5 (CH422G, mis à l'init).
#define CAN_TX 15
#define CAN_RX 16

static void can_task(void* arg)
{
    while (true)
    {
        twai_message_t rx;
        if (ESP_OK == twai_receive(&rx, pdMS_TO_TICKS(1000)))
        {
            ESP_LOGI(TAG, "CAN RX id=0x%08lX %s dlc=%d",
                     (unsigned long)rx.identifier, rx.extd ? "ext" : "std", rx.data_length_code);
        }
        // Trame étendue de test (visible sur un analyseur CAN).
        twai_message_t tx = {
            .extd = 1,                       // identifiant ÉTENDU (29 bits)
            .identifier = 0x12345678,
            .data_length_code = 2,
            .data = {0xAB, 0xCD},
        };
        twai_transmit(&tx, pdMS_TO_TICKS(100));
    }
}

void can_init(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX, (gpio_num_t)CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();   // accepte std ET étendus
    if (ESP_OK != twai_driver_install(&g, &t, &f))
    {
        ESP_LOGW(TAG, "TWAI : installation échouée");
        return;
    }
    if (ESP_OK != twai_start())
    {
        ESP_LOGW(TAG, "TWAI : démarrage échoué");
        return;
    }
    ESP_LOGI(TAG, "CAN 1 Mbit/s démarré (TX=%d RX=%d, IDs étendus)", CAN_TX, CAN_RX);
    xTaskCreate(can_task, "can", 3072, NULL, 5, NULL);
}
