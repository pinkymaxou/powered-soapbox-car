// main.cpp — Point d'entrée : initialise les sous-systèmes puis démarre le contrôle.
// Tout le contrôle des moteurs est dans controller.cpp.
#include "config.hpp"
#include "controller.hpp"
#include "hardware.hpp"
#include "leds.hpp"
#include "webserver.hpp"

#include "esp_log.h"
#include "nvs_flash.h"

extern "C" void app_main()
{
    board::powerLatch();   // EN PREMIER : tenir l'alimentation (le bouton externe est momentané)

    esp_err_t nv = nvs_flash_init();
    if (ESP_ERR_NVS_NO_FREE_PAGES == nv || ESP_ERR_NVS_NEW_VERSION_FOUND == nv)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    configInit();        // table de réglages (NVS)
    wifiSoftAPInit();    // point d'accès « Kart-Config »
    webServerStart();    // serveur HTTP/WebSocket
    kartInit();          // matériel (ADC, PWM, encodeurs, boutons)
    ledsStart();         // tâche d'affichage du ruban WS2812B
    kartStart();         // boucle de contrôle 100 Hz (système désarmé au démarrage)

    ESP_LOGI("kart", "Kart prêt. Config : Wi-Fi « Kart-Config » → http://192.168.4.1");
}
