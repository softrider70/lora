/*
 * config.h - Hardware-Konfiguration fuer Heltec WiFi LoRa 32 V3
 *
 * Board: Heltec WiFi LoRa 32 V3
 * Chip: ESP32-S3 (Dual-Core Xtensa LX7, 240MHz)
 * LoRa: SX1262 (868/915MHz)
 * Display: OLED SSD1306 128x64 (I2C)
 * Flash: 16MB, PSRAM: 8MB Octal
 */

#ifndef CONFIG_H
#define CONFIG_H


#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * Board-Identifikation
 * ===================================================================== */
#define BOARD_NAME              "Heltec WiFi LoRa 32 V3"
#define BOARD_CHIP              "ESP32-S3"
#define APP_VERSION_MAJOR       0
#define APP_VERSION_MINOR       1

/* =====================================================================
 * LoRa (SX1262) - SPI-Konfiguration
 * ===================================================================== */
#define LORA_SPI_HOST           SPI2_HOST
#define LORA_MOSI_GPIO          10
#define LORA_MISO_GPIO          11
#define LORA_SCLK_GPIO          9
#define LORA_NSS_GPIO           8
#define LORA_RST_GPIO           12
#define LORA_BUSY_GPIO          13
#define LORA_DIO1_GPIO          14

/* LoRa-Funkparameter (868 MHz EU-Band) */
#define LORA_FREQUENCY          868000000   /* 868 MHz */
#define LORA_BANDWIDTH          0           /* 0 = 125 kHz (BW125) */
#define LORA_SF                 7           /* Spreading Factor 7 */
#define LORA_CR                 1           /* Coding Rate 4/5 */
#define LORA_PREAMBLE_LENGTH    8
#define LORA_TX_POWER           14          /* dBm */

/* =====================================================================
 * OLED Display (SSD1306) - I2C-Konfiguration
 * ===================================================================== */
#define DISPLAY_I2C_PORT        I2C_NUM_0
#define DISPLAY_SDA_GPIO        41
#define DISPLAY_SCL_GPIO        42
#define DISPLAY_I2C_FREQ        400000      /* 400 kHz */
#define DISPLAY_I2C_ADDR        0x3C
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64

/* =====================================================================
 * LED (Heltec V3 Board-LED)
 * ===================================================================== */
#define LED_GPIO                35
#define LED_ON                  1
#define LED_OFF                 0

/* =====================================================================
 * Taster
 * ===================================================================== */
#define BUTTON_GPIO             0           /* BOOT-Taster */
#define BUTTON_DEBOUNCE_MS      50
#define BUTTON_LONGPRESS_MS     3000

/* =====================================================================
 * WiFi & OTA
 * ===================================================================== */
#define WIFI_AP_SSID            "LoRa-AP"
#define WIFI_AP_IP              "10.1.1.1"
#define WIFI_AP_NETMASK         "255.255.255.0"
#define WIFI_CONNECT_TIMEOUT_S  15
#define WIFI_MAX_RETRY          3

#define OTA_WEBSERVER_PORT      80
#define OTA_CHECK_INTERVAL_MS   60000       /* 60s */

/* =====================================================================
 * mDNS
 * ===================================================================== */
#define MDNS_HOSTNAME           "lora-node"

/* =====================================================================
 * LoRa-Nachricht-Typen (fuer Kommunikation zwischen Nodes)
 * ===================================================================== */
#define LORA_MSG_TYPE_SENSOR    0x01        /* Sensorwert */
#define LORA_MSG_TYPE_STATUS    0x02        /* Statusmeldung */
#define LORA_MSG_TYPE_PING      0x03        /* Ping */
#define LORA_MSG_TYPE_PONG      0x04        /* Pong */
#define LORA_MSG_TYPE_CONFIG    0x05        /* Konfiguration */
#define LORA_MSG_TYPE_ALARM     0x06        /* Alarm */

/* Maximale Nutzdatenlaenge einer LoRa-Nachricht */
#define LORA_MAX_PAYLOAD_LEN    240

/* =====================================================================
 * FreeRTOS Task-Konfiguration
 * ===================================================================== */
#define TASK_STACK_LORA         4096
#define TASK_STACK_DISPLAY      2048
#define TASK_STACK_WIFI         4096
#define TASK_STACK_OTA          4096
#define TASK_STACK_MONITOR      1024

#define TASK_PRIORITY_LORA      5
#define TASK_PRIORITY_DISPLAY   3
#define TASK_PRIORITY_WIFI      4
#define TASK_PRIORITY_OTA       3
#define TASK_PRIORITY_MONITOR   1

/* =====================================================================
 * Watchdog
 * ===================================================================== */
#define WDT_TIMEOUT_S           10

/* =====================================================================
 * Display-Timeout (Display ausschalten nach X Sekunden Inaktivitaet, 0=immer an)
 * ===================================================================== */
#define DISPLAY_TIMEOUT_S       0

/* =====================================================================
 * Sendeintervall (Millisekunden)
 * ===================================================================== */
#define LORA_SEND_INTERVAL_MS   5000

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */