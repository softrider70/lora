/*
 * lora.h - LoRa Kommunikation (SX1262) fuer Heltec WiFi LoRa 32 V3
 * Verwendet den SX126x-Treiber aus ESP-IDF (RadioLib) oder eigene Implementation
 */

#ifndef LORA_H
#define LORA_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LoRa-Nachricht-Struktur */
typedef struct {
    uint8_t type;           /* Nachrichtentyp (LORA_MSG_TYPE_*) */
    uint8_t node_id;        /* Sender-Knoten-ID */
    uint8_t payload[240];   /* Nutzdaten */
    uint16_t payload_len;   /* Laenge der Nutzdaten */
    int16_t rssi;           /* Empfangsstaerke (dBm) */
    int8_t snr;             /* Signal-Rausch-Verhaeltnis */
} lora_message_t;

/* LoRa-Treiber-Zustand */
typedef enum {
    LORA_STATE_IDLE = 0,
    LORA_STATE_TX,
    LORA_STATE_RX,
    LORA_STATE_ERROR
} lora_state_t;

/* Callback fuer empfangene Nachrichten */
typedef void (*lora_rx_callback_t)(const lora_message_t *msg);

/* LoRa initialisieren (SPI, Pins, SX1262-Konfiguration) */
esp_err_t lora_init(void);

/* LoRa deinitialisieren */
esp_err_t lora_deinit(void);

/* LoRa-Nachricht senden (blockierend, mit Timeout) */
esp_err_t lora_send(const lora_message_t *msg, uint32_t timeout_ms);

/* LoRa-Nachricht senden (nicht-blockierend, Callback bei fertig) */
esp_err_t lora_send_async(const lora_message_t *msg);

/* LoRa-Empfang starten (nicht-blockierend) */
esp_err_t lora_start_rx(void);

/* LoRa-Empfang stoppen */
esp_err_t lora_stop_rx(void);

/* Callback fuer empfangene Nachrichten registrieren */
esp_err_t lora_set_rx_callback(lora_rx_callback_t callback);

/* Aktuellen LoRa-Zustand abfragen */
lora_state_t lora_get_state(void);

/* Letzten RSSI-Wert abfragen */
int16_t lora_get_last_rssi(void);

/* Letzten SNR-Wert abfragen */
int8_t lora_get_last_snr(void);

/* SX1262-Chip-Temperatur auslesen (Celsius * 10) */
int16_t lora_get_temperature(void);

/* Test: Nachricht senden und auf Echo warten */
esp_err_t lora_ping(uint8_t target_node_id, uint32_t timeout_ms);

/* Node-ID setzen/lesen */
void lora_set_node_id(uint8_t node_id);
uint8_t lora_get_node_id(void);

#ifdef __cplusplus
}
#endif

#endif /* LORA_H */