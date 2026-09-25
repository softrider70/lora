idf_component_register(
    SRCS
        main.c
        nvs_config.c
        display.c
        heap_monitor.c
        stack_monitor.c
        lora.c
        wifi.c
        ota.c
        gps.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
    REQUIRES
        nvs_flash
        esp_timer
        esp_wifi
        esp_netif
        esp_http_server
        app_update
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
)

# ---------------------------------------------------------------------------
# Build-Zaehler: bei jedem Build eins hochzaehlen und include/version.h neu
# schreiben. main.c meldet BUILD_NUMBER im Log und auf dem Display, damit nach
# mehreren Flashvorgaengen erkennbar bleibt, welcher Stand laeuft.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    set(Python3_EXECUTABLE python)
endif()

add_custom_target(lora_version ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/increment_build.py
            --project-dir ${CMAKE_SOURCE_DIR}
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/include/version.h
    COMMENT "Build-Zaehler erhoehen und include/version.h erzeugen"
    VERBATIM
)

add_dependencies(${COMPONENT_LIB} lora_version)