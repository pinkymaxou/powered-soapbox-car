idf_component_register(
    SRCS "main.c" "wifi_web.c" "periph.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_i2c esp_wifi esp_event esp_netif
             nvs_flash esp_http_server
             esp_driver_sdmmc sdmmc fatfs driver
)

# Le driver TWAI « legacy » (driver/twai.h) émet un #warning de dépréciation traité comme
# erreur ; on l'autorise pour ce fichier (API legacy plus simple pour ce test).
set_source_files_properties(periph.c PROPERTIES COMPILE_FLAGS "-Wno-error=cpp")
