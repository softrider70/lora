/*
 * main.c - Hauptprogramm fuer Heltec WiFi LoRa 32 V3
 *
 * Initialisiert alle Komponenten und startet die FreeRTOS-Tasks:
 * - LoRa-Task (TX/RX im Intervall)
 * - Display-Task (Statusaktualisierung)
 * - Button-Task (BOOT-Taster)
 * - Monitoring-Tasks (Heap/Stack)
 *
 * Dual-Core: Core 0 = WiFi/OTA/Monitoring, Core 1 = LoRa/Display
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "config.h"
#include "lora.h"
#include "display.h"
#include "wifi.h"
#include "ota.h"
#include "nvs_config.h"
#include "heap_monitor.h"
#include "stack_monitor.h"
#include "gps.h"
#include "version.h"

static const char *TAG = "MAIN";

/* Task-Handles */
static TaskHandle_t lora_task_handle = NULL;
static TaskHandle_t display_task_handle = NULL;
static TaskHandle_t button_task_handle = NULL;

/* Eigene Node-ID */
static uint8_t node_id = 1;

/* LED-Blink-Status */
static bool led_state = false;

/* Letzte empfangene Nachricht (fuer Display) */
static lora_message_t last_rx_msg;
static bool last_rx_valid = false;

/* Sendezahler (von lora_task gesetzt, von display_task gelesen) */
static volatile uint32_t g_send_counter = 0;
static volatile int16_t g_last_tx_rssi = 0;
static volatile int8_t g_last_tx_snr = 0;
static volatile bool g_tx_active = false;

/* Zuletzt empfangene Position der Gegenseite (LORA_MSG_TYPE_GPS) */
static gps_data_t g_last_rx_gps;
static bool g_last_rx_gps_valid = false;

/* ====================================================================
 * GPS-Hilfsfunktionen
 * ==================================================================== */

/* Koordinate als Text: Vorzeichen, Grad und 7 Nachkommastellen (rund 1 cm).
 * Bewusst ohne %f, damit die Anzeige unabhaengig von printf-Optionen bleibt. */
static void format_coord(char *out, size_t len, double value)
{
    double shifted = (value * 10000000.0) + ((value < 0) ? -0.5 : 0.5);
    int32_t scaled = (int32_t)shifted;

    char sign = (scaled < 0) ? '-' : '+';
    int32_t abs_val = (scaled < 0) ? -scaled : scaled;

    snprintf(out, len, "%c%ld.%07ld", sign,
             (long)(abs_val / 10000000), (long)(abs_val % 10000000));
}

/* Position in 12 Byte verpacken: lat_i32, lon_i32, alt_i16, sats, hdop*10 */
static void gps_pack_payload(const gps_data_t *gps, uint8_t *buf, uint16_t *len)
{
    double lat_shift = (gps->latitude * 10000000.0) + ((gps->latitude < 0) ? -0.5 : 0.5);
    double lon_shift = (gps->longitude * 10000000.0) + ((gps->longitude < 0) ? -0.5 : 0.5);
    int32_t lat = (int32_t)lat_shift;
    int32_t lon = (int32_t)lon_shift;
    int16_t alt = (int16_t)gps->altitude_m;
    uint8_t hdop10 = (uint8_t)((gps->hdop * 10.0) + 0.5);

    buf[0] = (lat >> 24) & 0xFF;
    buf[1] = (lat >> 16) & 0xFF;
    buf[2] = (lat >> 8) & 0xFF;
    buf[3] = lat & 0xFF;
    buf[4] = (lon >> 24) & 0xFF;
    buf[5] = (lon >> 16) & 0xFF;
    buf[6] = (lon >> 8) & 0xFF;
    buf[7] = lon & 0xFF;
    buf[8] = (alt >> 8) & 0xFF;
    buf[9] = alt & 0xFF;
    buf[10] = gps->satellites;
    buf[11] = hdop10;

    *len = LORA_GPS_PAYLOAD_LEN;
}

/* 12 Byte wieder in Werte umwandeln (Rueckweg der Gegenseite) */
static bool gps_unpack_payload(const lora_message_t *msg, gps_data_t *out)
{
    if (!msg || msg->payload_len < LORA_GPS_PAYLOAD_LEN) {
        return false;
    }

    const uint8_t *b = msg->payload;
    int32_t lat = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                            ((uint32_t)b[2] << 8) | (uint32_t)b[3]);
    int32_t lon = (int32_t)(((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
                            ((uint32_t)b[6] << 8) | (uint32_t)b[7]);
    int16_t alt = (int16_t)(((uint16_t)b[8] << 8) | (uint16_t)b[9]);

    memset(out, 0, sizeof(*out));
    out->latitude = (double)lat / 10000000.0;
    out->longitude = (double)lon / 10000000.0;
    out->altitude_m = (double)alt;
    out->satellites = b[10];
    out->hdop = (double)b[11] / 10.0;
    out->valid = true;
    return true;
}

/* ====================================================================
 * LoRa-RX-Callback (wird aus dem DIO1-Task aufgerufen)
 * ==================================================================== */

static void lora_rx_callback_handler(const lora_message_t *msg)
{
    if (!msg) return;

    ESP_LOGI(TAG, "RX: Type=0x%02X, Node=%u, RSSI=%d, SNR=%d, Len=%u",
             msg->type, msg->node_id, msg->rssi, msg->snr, msg->payload_len);

    /* Nachricht kopieren fuer Display-Task */
    memcpy(&last_rx_msg, msg, sizeof(lora_message_t));
    last_rx_valid = true;

    /* Position der Gegenseite auswerten, damit die Anzeige sie zeigen kann */
    if (msg->type == LORA_MSG_TYPE_GPS) {
        if (gps_unpack_payload(msg, &g_last_rx_gps)) {
            char coord[20];
            format_coord(coord, sizeof(coord), g_last_rx_gps.latitude);
            g_last_rx_gps_valid = true;
            ESP_LOGI(TAG, "GPS von Node %u: %s (%u Sat)",
                     msg->node_id, coord, (unsigned)g_last_rx_gps.satellites);
        }
    }

    /* LED kurz einschalten */
    led_state = true;
    gpio_set_level(LED_GPIO, LED_ON);

    /* Button: auf PING mit PONG antworten */
    if (msg->type == LORA_MSG_TYPE_PING) {
        lora_message_t pong_msg;
        memset(&pong_msg, 0, sizeof(pong_msg));
        pong_msg.type = LORA_MSG_TYPE_PONG;
        pong_msg.node_id = node_id;
        pong_msg.payload[0] = msg->node_id;
        pong_msg.payload_len = 1;
        lora_send_async(&pong_msg);
        ESP_LOGI(TAG, "PONG gesendet an Node %u", msg->node_id);
    }
}

/* ====================================================================
 * LoRa-Task: Sendet periodisch Sensordaten
 * ==================================================================== */

static void lora_task(void *arg)
{
    ESP_LOGI(TAG, "LoRa-Task gestartet");

    lora_set_rx_callback(lora_rx_callback_handler);
    lora_start_rx();

    uint32_t counter = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LORA_SEND_INTERVAL_MS));

        gps_data_t gps;
        gps_get_data(&gps);

        lora_message_t tx_msg;
        memset(&tx_msg, 0, sizeof(tx_msg));
        tx_msg.node_id = node_id;

        if (gps.valid) {
            /* Position senden - der andere Node zeigt sie an */
            tx_msg.type = LORA_MSG_TYPE_GPS;
            gps_pack_payload(&gps, tx_msg.payload, &tx_msg.payload_len);
            ESP_LOGI(TAG, "Sende Position #%lu (%u Sat)",
                     (unsigned long)counter, (unsigned)gps.satellites);
        } else {
            /* Kein Fix: Lebenszeichen mit Zaehler und Chip-Temperatur */
            tx_msg.type = LORA_MSG_TYPE_STATUS;
            tx_msg.payload_len = 4;
            tx_msg.payload[0] = (counter >> 24) & 0xFF;
            tx_msg.payload[1] = (counter >> 16) & 0xFF;
            tx_msg.payload[2] = (counter >> 8) & 0xFF;
            tx_msg.payload[3] = counter & 0xFF;

            int16_t temp = lora_get_temperature();
            if (temp != 0 && tx_msg.payload_len + 2 <= LORA_MAX_PAYLOAD_LEN) {
                tx_msg.payload[tx_msg.payload_len++] = (temp >> 8) & 0xFF;
                tx_msg.payload[tx_msg.payload_len++] = temp & 0xFF;
            }

            ESP_LOGI(TAG, "Kein GPS-Fix - sende Status #%lu", (unsigned long)counter);
        }

        g_tx_active = true;
        lora_send(&tx_msg, 1000);
        g_send_counter = counter;
        g_last_tx_rssi = lora_get_last_rssi();
        g_last_tx_snr = lora_get_last_snr();
        g_tx_active = false;

        counter++;
    }
}

/* ====================================================================
 * Display-Task: Split-Screen mit eigenen + RX-Werten
 * ==================================================================== */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display-Task gestartet");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        char line[28];
        gps_data_t gps;
        gps_get_data(&gps);

        /* Zeile 0: Node-ID, Build-Nummer und GPS-Guete */
        if (gps.valid) {
            snprintf(line, sizeof(line), "N%u B%d %uSat", node_id, BUILD_NUMBER,
                     (unsigned)gps.satellites);
        } else {
            snprintf(line, sizeof(line), "N%u B%d kein Fix", node_id, BUILD_NUMBER);
        }
        display_show_line(0, line);

        /* Zeilen 1-3: eigene Position */
        if (gps.valid) {
            char coord[20];

            format_coord(coord, sizeof(coord), gps.latitude);
            snprintf(line, sizeof(line), "Lat %s", coord);
            display_show_line(1, line);

            format_coord(coord, sizeof(coord), gps.longitude);
            snprintf(line, sizeof(line), "Lon %s", coord);
            display_show_line(2, line);

            snprintf(line, sizeof(line), "Alt %dm HDOP %.1f",
                     (int)gps.altitude_m, gps.hdop);
            display_show_line(3, line);
        } else {
            display_show_line(1, "warte auf GPS-Fix");

            snprintf(line, sizeof(line), "Sat %u Baud %lu",
                     (unsigned)gps.satellites, (unsigned long)gps.baud);
            display_show_line(2, line);

            if (wifi_is_connected()) {
                snprintf(line, sizeof(line), "IP: %s", wifi_get_ip());
                display_show_line(3, line);
            } else {
                display_show_line(3, "WiFi: AP");
            }
        }

        /* Zeilen 4-5: eigener Funkstatus */
        snprintf(line, sizeof(line), "TX#%lu %ddBm SN%ddB",
                 (unsigned long)g_send_counter, (int)g_last_tx_rssi,
                 (int)g_last_tx_snr);
        display_show_line(4, line);

        if (last_rx_valid) {
            snprintf(line, sizeof(line), "RX N%u %ddBm",
                     last_rx_msg.node_id, (int)last_rx_msg.rssi);
        } else {
            snprintf(line, sizeof(line), "RX --");
        }
        display_show_line(5, line);

        /* Zeilen 6-7: Position der Gegenseite */
        if (g_last_rx_gps_valid) {
            char coord[20];

            format_coord(coord, sizeof(coord), g_last_rx_gps.latitude);
            snprintf(line, sizeof(line), "Lat %s", coord);
            display_show_line(6, line);

            format_coord(coord, sizeof(coord), g_last_rx_gps.longitude);
            snprintf(line, sizeof(line), "Lon %s", coord);
            display_show_line(7, line);
        } else if (last_rx_valid) {
            snprintf(line, sizeof(line), "Typ 0x%02X Len %u",
                     last_rx_msg.type, last_rx_msg.payload_len);
            display_show_line(6, line);
            display_show_line(7, "keine Position");
        } else {
            display_show_line(6, "RX: --");
            display_show_line(7, "warte auf Node");
        }
    }
}

/* ====================================================================
 * Button-Task: BOOT-Taster (GPIO0) ueberwachen
 * ==================================================================== */

static void button_task(void *arg)
{
    ESP_LOGI(TAG, "Button-Task gestartet");

    uint32_t press_start = 0;
    bool was_pressed = false;

    while (1) {
        bool pressed = (gpio_get_level(BUTTON_GPIO) == 0);

        if (pressed && !was_pressed) {
            press_start = xTaskGetTickCount();
            was_pressed = true;
        } else if (pressed && was_pressed) {
            uint32_t duration = pdTICKS_TO_MS(xTaskGetTickCount() - press_start);
            if (duration >= BUTTON_LONGPRESS_MS) {
                ESP_LOGW(TAG, "Langer Tastendruck (%lu ms) - Reset WiFi!", duration);
                display_show_status("WiFi Reset...");
                vTaskDelay(pdMS_TO_TICKS(500));
                wifi_reset_credentials();
                esp_restart();
            }
        } else if (!pressed && was_pressed) {
            uint32_t duration = pdTICKS_TO_MS(xTaskGetTickCount() - press_start);
            if (duration < BUTTON_LONGPRESS_MS && duration > BUTTON_DEBOUNCE_MS) {
                ESP_LOGI(TAG, "Kurzer Tastendruck (%lu ms) - Ping an Node 2", duration);
                lora_ping(2, 1000);
            }
            was_pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ====================================================================
 * LED-Blink-Timer
 * ==================================================================== */

static void led_timer_callback(TimerHandle_t xTimer)
{
    if (led_state) {
        gpio_set_level(LED_GPIO, LED_OFF);
        led_state = false;
    }
}

/* ====================================================================
 * app_main
 * ==================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "=== LoRa Node %s ===", APP_VERSION_STRING);
    ESP_LOGI(TAG, "Board: %s (%s)", BOARD_NAME, BOARD_CHIP);
    ESP_LOGI(TAG, "Build %d vom %s", BUILD_NUMBER, BUILD_TIMESTAMP);

    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, LED_OFF);

    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    ESP_ERROR_CHECK(nvs_config_init());

    /* Node-ID aus MAC-Adresse ableiten (letztes Byte der MAC, 1-254)
     * Ueberschreibbar via NVS (Captive Portal oder spaeter per API) */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint8_t mac_node_id = (mac[5] % 254) + 1; /* 1..254 */

    node_id = nvs_config_get_u8("node_id", mac_node_id);
    if (node_id == 0) node_id = mac_node_id; /* 0 ist ungueltig */

    ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X -> Node-ID: %u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], node_id);

    /* Node-ID auch dem Funk-Treiber mitteilen (nutzt er fuer Ping/Pong) */
    lora_set_node_id(node_id);

    esp_err_t ret = display_init();
    if (ret == ESP_OK) {
        display_show_splash();
        vTaskDelay(pdMS_TO_TICKS(2000));
        display_show_status("Initialisiere...");
    } else {
        ESP_LOGW(TAG, "Display-Init fehlgeschlagen: %s", esp_err_to_name(ret));
    }

    ret = lora_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LoRa-Init fehlgeschlagen: %s", esp_err_to_name(ret));
    }

    ret = gps_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPS-Init fehlgeschlagen: %s", esp_err_to_name(ret));
    }

    ret = wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi-Init fehlgeschlagen: %s", esp_err_to_name(ret));
    }

    ret = ota_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA-Init fehlgeschlagen: %s", esp_err_to_name(ret));
    }

    heap_monitor_init();
    stack_monitor_init();

    TimerHandle_t led_timer = xTimerCreate(
        "led_blink", pdMS_TO_TICKS(100), pdFALSE, NULL, led_timer_callback);
    if (led_timer) {
        xTimerStart(led_timer, 0);
    }

    xTaskCreatePinnedToCore(
        lora_task, "lora", TASK_STACK_LORA, NULL,
        TASK_PRIORITY_LORA, &lora_task_handle, 1);

    xTaskCreatePinnedToCore(
        display_task, "display", TASK_STACK_DISPLAY, NULL,
        TASK_PRIORITY_DISPLAY, &display_task_handle, 1);

    xTaskCreatePinnedToCore(
        button_task, "button", TASK_STACK_BUTTON, NULL,
        TASK_PRIORITY_MONITOR, &button_task_handle, 0);

    ESP_LOGI(TAG, "Initialisierung abgeschlossen");

    gpio_set_level(LED_GPIO, LED_ON);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED_GPIO, LED_OFF);

    /* Bewusst die letzte Meldung: zeigt im Log den laufenden Stand */
    ESP_LOGI(TAG, "%s bereit - Build %d", BOARD_NAME, BUILD_NUMBER);
}