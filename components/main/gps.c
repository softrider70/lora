/*
 * gps.c - GPS-Empfaenger NEO-6M (GY-NE06MV2) ueber UART
 *
 * Aufgaben:
 *  - UART1 auf GPIO4 (RX) / GPIO5 (TX) einrichten
 *  - Baudrate automatisch finden (9600 zuerst, dann 38400/57600/115200/4800)
 *  - NMEA-0183-Saetze lesen, Pruefsumme kontrollieren und GGA auswerten
 *  - Ergebnis threadsicher bereitstellen (gps_get_data)
 *
 * Es wird nur GGA gebraucht: darin stehen Position, Hoehe, Satelliten und HDOP.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "config.h"
#include "gps.h"

static const char *TAG = "GPS";

#define NMEA_LINE_MAX   100     /* laengste NMEA-Zeile inkl. Ende */
#define NMEA_MAX_FIELDS 20

/* Baudraten, die der NEO-6M ab Werk kennt (9600 ist Standard) */
static const uint32_t s_baud_table[] = { 9600, 38400, 57600, 115200, 4800 };
#define BAUD_COUNT (sizeof(s_baud_table) / sizeof(s_baud_table[0]))

static SemaphoreHandle_t s_mutex = NULL;
static gps_data_t s_data = { 0 };
static TaskHandle_t s_task = NULL;
static bool s_initialized = false;

static int64_t s_last_fix_us = 0;
static bool s_sentence_seen = false;    /* mindestens eine Zeile kam an */
static int s_baud_index = 0;

/* ====================================================================
 * NMEA-Hilfsfunktionen
 * ==================================================================== */

/* Pruefsumme: XOR aller Zeichen zwischen '$' und '*' */
static bool nmea_checksum_ok(const char *line)
{
    const char *star = strchr(line, '*');
    if (!star || line[0] != '$' || star == line + 1) {
        return false;
    }

    uint8_t calc = 0;
    for (const char *p = line + 1; p < star; p++) {
        calc ^= (uint8_t)(*p);
    }

    /* Pruefsumme braucht genau zwei Hex-Zeichen - sonst Zeile verwerfen */
    if (star[1] == '\0' || star[2] == '\0') {
        return false;
    }
    char given_str[3] = { star[1], star[2], '\0' };
    uint8_t given = (uint8_t)strtoul(given_str, NULL, 16);
    return calc == given;
}

/* Zeile an den Kommas zerlegen (aendert den Puffer) */
static int nmea_split(char *body, char *fields[], int max)
{
    int count = 0;
    char *p = body;

    while (count < max) {
        fields[count++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    return count;
}

/* Zahl aus Feld lesen; Feld darf leer sein */
static bool nmea_number(const char *field, double *out)
{
    if (!field || field[0] == '\0') {
        return false;
    }
    char *end = NULL;
    double value = strtod(field, &end);
    if (end == field) {
        return false;
    }
    *out = value;
    return true;
}

/* NMEA-Gradminuten (ddmm.mmmm) in Dezimalgrad umrechnen */
static double nmea_to_degrees(double raw, char dir)
{
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double value = degrees + (minutes / 60.0);

    if (dir == 'S' || dir == 'W') {
        value = -value;
    }
    return value;
}

/* ====================================================================
 * GGA-Satz auswerten
 *   Feld 1  UTC-Zeit        Feld 2/3 Breite + N/S
 *   Feld 4/5 Laenge + E/W   Feld 6 Qualitaet (0 = kein Fix)
 *   Feld 7   Satelliten     Feld 8 HDOP        Feld 9 Hoehe
 * ==================================================================== */
static void parse_gga(char *fields[], int count)
{
    if (count < 10) {
        return;
    }

    double raw_lat = 0, raw_lon = 0, quality = 0, sats = 0, hdop = 0, alt = 0;
    bool has_lat = nmea_number(fields[2], &raw_lat);
    bool has_lon = nmea_number(fields[4], &raw_lon);

    nmea_number(fields[6], &quality);
    nmea_number(fields[7], &sats);
    nmea_number(fields[8], &hdop);
    nmea_number(fields[9], &alt);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_data.sentences++;
    s_data.quality = (quality > 0xFF) ? 0xFF : (uint8_t)quality;
    strncpy(s_data.utc_time, fields[1], sizeof(s_data.utc_time) - 1);
    s_data.utc_time[sizeof(s_data.utc_time) - 1] = '\0';

    if (has_lat && has_lon && quality > 0) {
        s_data.latitude = nmea_to_degrees(raw_lat, fields[3][0]);
        s_data.longitude = nmea_to_degrees(raw_lon, fields[5][0]);
        s_data.altitude_m = alt;
        s_data.satellites = (uint8_t)sats;
        s_data.hdop = hdop;
        s_data.valid = true;
        s_last_fix_us = esp_timer_get_time();
    } else {
        /* Kein Fix: Position bleibt stehen, aber als ungueltig markiert */
        s_data.satellites = (uint8_t)sats;
        s_data.hdop = hdop;
        s_data.valid = false;
    }

    xSemaphoreGive(s_mutex);
}

/* Eine vollstaendige NMEA-Zeile verarbeiten */
static void gps_handle_line(char *line)
{
    if (line[0] != '$') {
        return;
    }

    if (!nmea_checksum_ok(line)) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_data.errors++;
        xSemaphoreGive(s_mutex);
        return;
    }

    /* Zeile in Arbeitskopie umwandeln: '$' und Pruefsumme abschneiden */
    char body[NMEA_LINE_MAX];
    strncpy(body, line + 1, sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
    char *star = strchr(body, '*');
    if (star) {
        *star = '\0';
    }

    /* Sprecher (GP/GN/GL) und Satzart pruefen */
    if (strlen(body) < 5) {
        return;
    }
    const char *type = body + 2;

    if (strncmp(type, "GGA", 3) == 0) {
        char *fields[NMEA_MAX_FIELDS];
        int count = nmea_split(body, fields, NMEA_MAX_FIELDS);
        parse_gga(fields, count);
    }
}

/* ====================================================================
 * Empfangs-Task
 * ==================================================================== */

static void gps_task(void *arg)
{
    uint8_t chunk[128];
    char line[NMEA_LINE_MAX];
    size_t line_len = 0;
    bool baud_locked = false;
    int64_t last_status_us = 0;

    ESP_LOGI(TAG, "GPS-Task gestartet (RX=GPIO%d, TX=GPIO%d)",
             GPS_RX_GPIO, GPS_TX_GPIO);

    while (1) {
        int n = uart_read_bytes(GPS_UART_PORT, chunk, sizeof(chunk),
                                pdMS_TO_TICKS(300));

        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];

            if (c == '\n' || c == '\r') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    if (line[0] == '$') {
                        if (!s_sentence_seen) {
                            s_sentence_seen = true;
                            baud_locked = true;
                            ESP_LOGI(TAG, "NMEA-Empfang bei %lu Baud",
                                     (unsigned long)s_baud_table[s_baud_index]);
                        }
                        gps_handle_line(line);
                    }
                    line_len = 0;
                }
            } else if (line_len < NMEA_LINE_MAX - 1) {
                line[line_len++] = c;
            } else {
                line_len = 0;   /* ueberlange Zeile verwerfen */
            }
        }

        /* Baudrate suchen, solange noch keine Zeile ankam */
        if (n == 0 && !baud_locked && !s_sentence_seen) {
            s_baud_index = (s_baud_index + 1) % BAUD_COUNT;
            uint32_t baud = s_baud_table[s_baud_index];
            uart_set_baudrate(GPS_UART_PORT, baud);
            uart_flush_input(GPS_UART_PORT);

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_data.baud = baud;
            xSemaphoreGive(s_mutex);

            /* Nur einmal pro vollem Durchlauf melden - sonst steht das Log
             * alle 300 ms voll. Ein Durchlauf endet bei Index 0. */
            if (s_baud_index == 0) {
                ESP_LOGW(TAG, "Kein GPS angeschlossen oder stumm - "
                              "alle Baudraten ohne Daten");
            } else {
                ESP_LOGD(TAG, "Teste %lu Baud", (unsigned long)baud);
            }
        }

        /* Alle 10 s den Stand melden: Anzahl ausgewerteter Saetze, Fehler,
         * Satelliten und Qualitaet. Damit ist belegbar, ob NMEA ankommt und ob
         * der Empfaenger schon einen Fix hat. */
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_status_us >= 10 * 1000 * 1000) {
            last_status_us = now_us;

            gps_data_t stand;
            gps_get_data(&stand);
            ESP_LOGI(TAG, "NMEA: %lu Saetze, %lu Fehler, Sat %u, Qual %u, HDOP %u.%u",
                     (unsigned long)stand.sentences, (unsigned long)stand.errors,
                     (unsigned)stand.satellites, (unsigned)stand.quality,
                     (unsigned)((int)(stand.hdop * 10.0) / 10),
                     (unsigned)((int)(stand.hdop * 10.0) % 10));
        }
    }
}

/* ====================================================================
 * Oeffentliche API
 * ==================================================================== */

esp_err_t gps_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_baud_index = 0;
    s_data.baud = s_baud_table[0];

    uart_config_t cfg = {
        .baud_rate = (int)s_baud_table[0],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(GPS_UART_PORT, GPS_RX_BUFFER_SIZE,
                                        0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART-Treiber-Install fehlgeschlagen: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(GPS_UART_PORT, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART-Konfiguration fehlgeschlagen: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(GPS_UART_PORT, GPS_TX_GPIO, GPS_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART-Pins setzen fehlgeschlagen: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = uart_flush_input(GPS_UART_PORT);
    if (ret != ESP_OK) {
        return ret;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(gps_task, "gps", GPS_TASK_STACK,
                                            NULL, GPS_TASK_PRIORITY,
                                            &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "GPS-Task konnte nicht angelegt werden");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "UART%d bereit (RX=GPIO%d, TX=GPIO%d, %lu Baud)",
             GPS_UART_PORT, GPS_RX_GPIO, GPS_TX_GPIO,
             (unsigned long)s_baud_table[0]);
    return ESP_OK;
}

void gps_get_data(gps_data_t *out)
{
    if (!out) {
        return;
    }

    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_mutex);

    if (s_last_fix_us > 0) {
        out->fix_age_ms = (uint32_t)((esp_timer_get_time() - s_last_fix_us) / 1000);
    }
}

bool gps_has_fix(void)
{
    if (!s_mutex) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool valid = s_data.valid;
    xSemaphoreGive(s_mutex);
    return valid;
}
