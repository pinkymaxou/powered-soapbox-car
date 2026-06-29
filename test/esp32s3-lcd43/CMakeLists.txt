# Firmware de TEST — ESP32-S3 + écran tactile RGB 4,3" (800×480).
# But : afficher un carré rouge en bas-gauche via LVGL pour valider la dalle.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lcd43_test)
