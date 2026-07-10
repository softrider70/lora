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

/* ====================================================================
 * LoRa-RX-Callback (wird aus Interrupt-Kontext aufgerufen)
 * ==================================================================== */

static void lora_rx_callback_handler(const lora_message_t *msg)
{
    if (!msg) return;

    ESP_LOGI(TAG, "RX: Type=0x%02X, Node=%u, RSSI=%d, SNR=%d, Len=%u",
             msg->type, msg->node_id, msg->rssi, msg->snr, msg->payload_len);

    /* Nachricht kopieren fuer Display-Task */
    memcpy(&last_rx_msg, msg, sizeof(lora_message_t));
    last_rx_valid = true;

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

        g_tx_active = true;

        lora_message_t tx_msg;
        memset(&tx_msg, 0, sizeof(tx_msg));
        tx_msg.type = LORA_MSG_TYPE_STATUS;
        tx_msg.node_id = node_id;
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

        ESP_LOGI(TAG, "Sende Status #%lu", counter);
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

        /* === Zeilen 0-1: eigener Status (Titel) === */
        char line[28];

        /* Zeile 0: Node-ID + Sendezaehler */
        snprintf(line, sizeof(line), "N%u Snd #%lu", node_id, g_send_counter);
        display_show_line(0, line);

        /* Zeile 1: Eigener RSSI/SNR */
        snprintf(line, sizeof(line), "S: %ddBm SNR:%ddB",
                 (int)g_last_tx_rssi, (int)g_last_tx_snr);
        display_show_line(1, line);

        /* === Zeilen 2-3: Trennlinie + WiFi === */
        if (wifi_is_connected()) {
            snprintf(line, sizeof(line), "IP: %s", wifi_get_ip());
            display_show_line(2, line);
        } else {
            display_show_line(2, "WiFi: AP");
        }

        /* === Zeilen 4-7: Gegenseite (RX) === */
        if (last_rx_valid) {
            /* Typ als Text */
            const char *type_str = "?";
            switch (last_rx_msg.type) {
                case LORA_MSG_TYPE_SENSOR: type_str = "Sensor"; break;
                case LORA_MSG_TYPE_STATUS: type_str = "Status"; break;
                case LORA_MSG_TYPE_PING:   type_str = "Ping";   break;
                case LORA_MSG_TYPE_PONG:   type_str = "Pong";   break;
                case LORA_MSG_TYPE_ALARM:  type_str = "Alarm";  break;
            }

            snprintf(line, sizeof(line), "RX: %s N%u", type_str, last_rx_msg.node_id);
            display_show_line(4, line);

            snprintf(line, sizeof(line), "G: %ddBm SNR:%ddB",
                     (int)last_rx_msg.rssi, (int)last_rx_msg.snr);
            display_show_line(5, line);

            /* Payload als Hex */
            char hex[40] = {0};
            int pos = 0;
            for (int i = 0; i < last_rx_msg.payload_len && i < 6 && pos < 38; i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", last_rx_msg.payload[i]);
            }
            if (pos > 0) {
                display_show_line(6, hex);
            }

            snprintf(line, sizeof(line), "Len: %u", last_rx_msg.payload_len);
            display_show_line(7, line);
        } else {
            display_show_line(4, "RX: --");
            display_show_line(5, "Warte auf Node...");
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
    ESP_LOGI(TAG, "=== LoRa Node v%d.%d ===", APP_VERSION_MAJOR, APP_VERSION_MINOR);
    ESP_LOGI(TAG, "Board: %s (%s)", BOARD_NAME, BOARD_CHIP);

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
    node_id = nvs_config_get_u8("node_id", 1);
    ESP_LOGI(TAG, "Node-ID: %u", node_id);

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
        button_task, "button", TASK_STACK_MONITOR, NULL,
        TASK_PRIORITY_MONITOR, &button_task_handle, 0);

    ESP_LOGI(TAG, "Initialisierung abgeschlossen");
    display_show_status("Bereit!");

    gpio_set_level(LED_GPIO, LED_ON);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED_GPIO, LED_OFF);
}