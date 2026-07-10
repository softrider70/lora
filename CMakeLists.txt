cmake_minimum_required(VERSION 3.24)

# ESP-IDF Projekt - LoRa Kommunikation (Heltec WiFi LoRa 32 V3)
set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lora)