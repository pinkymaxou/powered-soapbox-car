idf_component_register(
    SRCS
        "main.cpp"
        "controller.cpp"
        "ads1115.cpp"
        "hardware.cpp"
        "input.cpp"
        "input_bp32.c"
        "leds.cpp"
        "config.cpp"
        "webserver.cpp"
        "proto/kart.pb.c"
        "ws2812.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        nanopb
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2c
        esp_driver_rmt
        esp_app_format
        spi_flash
        nvs_flash
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        bluepad32
        btstack
)

# Assets web : compressés en gzip au build puis embarqués en binaire.
# Le serveur les renvoie tels quels avec l'en-tête « Content-Encoding: gzip »
# (le navigateur décompresse). Réduit fortement la flash (Chart.js 205 ko → ~70 ko).
foreach(asset index.html style.css chart.min.js pb.min.js)
    set(_src "${COMPONENT_DIR}/assets/${asset}")
    set(_gz  "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(
        OUTPUT  "${_gz}"
        COMMAND gzip -9 -n -c "${_src}" > "${_gz}"
        DEPENDS "${_src}"
        COMMENT "gzip ${asset}"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${_gz}" BINARY DEPENDS "${_gz}")
endforeach()
