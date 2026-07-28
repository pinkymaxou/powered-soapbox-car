// main.cpp — Entry point: initializes the subsystems then starts the control.
// All the motor control is in controller.cpp.
#include "config.hpp"
#include "controller.hpp"
#include "hardware.hpp"
#include "leds.hpp"
#include "mdns_svc.hpp"
#include "webserver.hpp"

#include "esp_log.h"
#include "nvs_flash.h"

extern "C" void app_main()
{
    board::motorsIdleEarly();  // FIRST: motor pins at rest (avoids any jolt at boot)
    board::powerLatch();       // hold the power (the external button is momentary)

    const esp_err_t nv = nvs_flash_init();
    if (ESP_ERR_NVS_NO_FREE_PAGES == nv || ESP_ERR_NVS_NEW_VERSION_FOUND == nv)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    configInit();        // settings table (NVS)
    wifiSoftAPInit();    // "Kart-Config" access point
    webServerStart();    // HTTP/WebSocket server
    mdnsStart();         // advertises http://kart.local (after the server: the port is open)
    Controller::init();  // hardware (ADC, PWM, I2C sensor, buttons)
    ledsStart();         // WS2812B strip display task
    Controller::start(); // 500 Hz control loop (system disarmed at startup)

    ESP_LOGI("kart", "Kart ready. Config: Wi-Fi 'Kart-Config' → http://%s.local (or http://192.168.4.1)",
             mdnsHostname());
}
