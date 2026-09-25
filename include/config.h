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
 *
 * ACHTUNG: hier standen GPIO41/GPIO42. Das sind beim ESP32-S3 die
 * JTAG-Pins MTDI/MTMS, nicht die Display-Leitungen. Belegt am 2026-09-25:
 * Display-Init meldete damit "ESP_ERR_INVALID_RESPONSE" (kein ACK auf 0x3C).
 * Das OLED haengt intern an GPIO17 (SDA) / GPIO18 (SCL); RST liegt auf GPIO21.
 * ===================================================================== */
#define DISPLAY_I2C_PORT        I2C_NUM_0
#define DISPLAY_SDA_GPIO        17
#define DISPLAY_SCL_GPIO        18
#define DISPLAY_RST_GPIO        21
#define DISPLAY_VEXT_GPIO       36          /* Vext_Ctrl, LOW = Display versorgt */

/* Gegenprobe fuer den I2C-Scan, wenn die Display-Init fehlschlaegt:
 * das war die Belegung der ersten Fassung. */
#define DISPLAY_ALT_SDA_GPIO    41
#define DISPLAY_ALT_SCL_GPIO    42
#define DISPLAY_I2C_FREQ        400000      /* 400 kHz */
#define DISPLAY_I2C_ADDR        0x3C
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64

/* =====================================================================
 * LED (Heltec V3 Board-LED)
 * Die Ansteuerung ist stillgelegt (2026-09-25): Der Pin wird nur noch als
 * Ausgang auf LED_OFF gehalten, die LED bleibt aus. Vorher leuchtete sie
 * dauerhaft, weil der Blink-Timer nie neu gestartet wurde.
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
 * GPS (GY-NE06MV2 mit u-blox NEO-6M) - UART1
 *
 * Anschluss am Heltec WiFi LoRa 32 V3:
 *   GPS-TX  -> GPIO4  (ESP-Empfang)   Header J3 Pin 15
 *   GPS-RX  -> GPIO5  (ESP-Senden)    Header J3 Pin 16
 *   VCC     -> 5V oder 3V3            J2 Pin 2 / J3 Pin 2-3
 *   GND     -> GND                    J3 Pin 1
 * GPIO4/5 sind frei, keine Strapping-Pins. Belegt sind bereits:
 * LoRa 8-14, OLED 17/18 + RST 21, LED 35, Vext 36, ADC_Ctrl 37,
 * Taster 0, Konsole 43/44, USB 19/20, interner Flash 26-32.
 * ===================================================================== */
#define GPS_UART_PORT           1           /* UART_NUM_1 */
#define GPS_TX_GPIO             5           /* ESP sendet -> GPS-RX */
#define GPS_RX_GPIO             4           /* GPS sendet -> ESP-RX */
#define GPS_RX_BUFFER_SIZE      1024
#define GPS_TASK_STACK          3584
#define GPS_TASK_PRIORITY       4

/* =====================================================================
 * WiFi & OTA
 * ===================================================================== */
#define WIFI_AP_SSID            "LoRa-AP"
#define WIFI_AP_IP              "10.1.1.1"
#define WIFI_AP_NETMASK         "255.255.255.0"
#define WIFI_CONNECT_TIMEOUT_S  15
#define WIFI_MAX_RETRY          3

#define OTA_WEBSERVER_PORT      8080        /* 80 belegt das Captive Portal */
#define OTA_CTRL_PORT           32769       /* 32768 belegt das Captive Portal */
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
#define LORA_MSG_TYPE_GPS       0x07        /* GPS-Position (12 Byte binaer) */

/* Laenge der GPS-Nutzlast in Bytes (siehe gps_build_payload in main.c) */
#define LORA_GPS_PAYLOAD_LEN    12

/* Maximale Nutzdatenlaenge einer LoRa-Nachricht */
#define LORA_MAX_PAYLOAD_LEN    240

/* =====================================================================
 * FreeRTOS Task-Konfiguration
 * ===================================================================== */
#define TASK_STACK_LORA         4096
#define TASK_STACK_DISPLAY      4096        /* 2048 war zu knapp - Stack-Overflow */
#define TASK_STACK_WIFI         4096
#define TASK_STACK_OTA          4096

/* Stackgroesse der Monitor-Tasks in Bytes. Beide Tasks benutzten vorher
 * configMINIMAL_STACK_SIZE + 256/512; das sind Bytes, nicht Woerter, und zu
 * wenig. Belegt am 2026-09-25: "A stack overflow in task stk_mon". */
#define TASK_STACK_MONITOR      3072

/* Der Button-Task benutzte TASK_STACK_MONITOR (1024 Byte). Das ist zu wenig:
 * er loggt, zeichnet aufs Display und schreibt ins NVS. Belegt am 2026-09-25 -
 * "A stack overflow in task button has been detected" direkt nach dem Start,
 * danach Neustart-Schleife (Rebooting...). */
#define TASK_STACK_BUTTON       4096

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