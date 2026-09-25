/*
 * gps.h - GPS-Empfaenger (u-blox NEO-6M, Modul GY-NE06MV2) am Heltec V3
 *
 * Anschluss (UART1):
 *   GPS-TX  -> ESP GPIO4  (ESP-Empfang)
 *   GPS-RX  -> ESP GPIO5  (ESP-Senden, optional)
 *   VCC     -> 5V (J2 Pin 2) oder 3V3 (J3 Pin 2/3)
 *   GND     -> GND
 *
 * Das Modul sendet NMEA-0183 mit 9600 Baud (8N1). Die Baudrate wird zur
 * Laufzeit gesucht, weil ein NEO-6M mit Backup-Batterie eine geaenderte
 * Baudrate behaelt.
 */

#ifndef GPS_H
#define GPS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Aktueller Stand des Empfaengers */
typedef struct {
    bool valid;             /* true = Fix mit Qualitaet > 0 */
    double latitude;        /* Grad, positiv = Nord */
    double longitude;       /* Grad, positiv = Ost */
    double altitude_m;      /* Hoehe ueber NN in Metern */
    uint8_t satellites;     /* Anzahl benutzter Satelliten */
    double hdop;            /* Guete der horizontalen Position */
    char utc_time[11];      /* UTC-Zeit aus NMEA, z. B. "123456.00" */
    uint32_t fix_age_ms;    /* Alter des letzten gueltigen Fix in ms */
    uint32_t sentences;     /* ausgewertete NMEA-Saetze */
    uint32_t errors;        /* Saetze mit falscher Pruefsumme */
    uint32_t baud;          /* aktuell benutzte Baudrate */
} gps_data_t;

/* UART starten und Empfangs-Task anlegen */
esp_err_t gps_init(void);

/* Stand abfragen (threadsicher) */
void gps_get_data(gps_data_t *out);

/* Kurzform: liegt ein gueltiger Fix vor? */
bool gps_has_fix(void);

#ifdef __cplusplus
}
#endif

#endif /* GPS_H */
