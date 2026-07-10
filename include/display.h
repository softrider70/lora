/*
 * display.h - OLED Display (SSD1306) Steuerung fuer Heltec WiFi LoRa 32 V3
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"
#include "lora.h"


#ifdef __cplusplus
extern "C" {
#endif

/* Display initialisieren (I2C, SSD1306) */
esp_err_t display_init(void);

/* Deinitialisieren */
esp_err_t display_deinit(void);

/* Splash-Screen anzeigen */
void display_show_splash(void);

/* Status-Text anzeigen */
void display_show_status(const char *status);

/* Fehler anzeigen */
void display_show_error(const char *error);

/* Empfangene LoRa-Nachricht anzeigen */
void display_show_lora_rx(const lora_message_t *msg);

/* Gesendete LoRa-Nachricht anzeigen */
void display_show_lora_tx(const lora_message_t *msg);

/* Text in Zeile X anzeigen (0-7 Zeilen bei 8px Font) */
void display_show_line(int line, const char *text);

/* Display clear */
void display_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */