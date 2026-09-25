/*
 * lora.c - SX1262 LoRa-Treiber fuer Heltec WiFi LoRa 32 V3
 *
 * SPI: MOSI=GPIO10, MISO=GPIO11, SCLK=GPIO9, NSS=GPIO8,
 *       RST=GPIO12, BUSY=GPIO13, DIO1=GPIO14
 * Frequenz: 868 MHz (EU-Band), SF=7, BW=125kHz
 *
 * Minimaler SX1262-Treiber ueber ESP-IDF SPI Master.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "config.h"
#include "lora.h"

static const char *TAG = "LORA";

/* SX1262 Kommandos */
#define SX1262_CMD_NOP                   0x00
#define SX1262_CMD_SET_SLEEP             0x84
#define SX1262_CMD_SET_STANDBY           0x80
#define SX1262_CMD_SET_FS                0xC1
#define SX1262_CMD_SET_TX                0x83
#define SX1262_CMD_SET_RX                0x82
#define SX1262_CMD_SET_PACKETTYPE        0x8A
#define SX1262_CMD_SET_RFFREQUENCY       0x86
#define SX1262_CMD_SET_TXPARAMS          0x8E
#define SX1262_CMD_SET_PAOCONFIG         0x95
#define SX1262_CMD_SET_LORAMODPARAMS     0x8B
#define SX1262_CMD_SET_LORAPKTPARAMS     0x8C
#define SX1262_CMD_GET_IRQSTATUS         0x12
#define SX1262_CMD_CLR_IRQSTATUS         0x02
#define SX1262_CMD_GET_RXBUFFERSTATUS    0x13
#define SX1262_CMD_GET_PACKETSTATUS      0x14
#define SX1262_CMD_SET_DIOIRQPARAMS      0x08
#define SX1262_CMD_SET_DIO2_AS_RF_SWITCH 0x9D
#define SX1262_CMD_SET_DIO3_AS_TCXO_CTRL 0x97
#define SX1262_CMD_CALIBRATE             0x89
#define SX1262_CMD_CALIBRATE_IMAGE       0x98
#define SX1262_CMD_CLEAR_DEVICE_ERRORS   0x07
#define SX1262_CMD_GET_DEVICE_ERRORS     0x17
#define SX1262_CMD_SET_REGULATOR_MODE    0x96
#define SX1262_CMD_WRITE_REGISTER        0x0D
#define SX1262_CMD_READ_REGISTER         0x1D
#define SX1262_CMD_WRITE_BUFFER          0x0E
#define SX1262_CMD_READ_BUFFER           0x1E

/* SX1262 Register */
#define SX1262_REG_LORA_SYNCWORD         0x0740
#define SX1262_REG_LORA_DETECTION_OPT    0x08B8
#define SX1262_REG_LORA_DETECTION_TH     0x08B9
#define SX1262_REG_TX_CLAMP_CURRENT      0x08D8
#define SX1262_REG_OCP_CONFIG            0x08E7

/* Interrupt-Masken */
#define SX1262_IRQ_TX_DONE               (1 << 0)
#define SX1262_IRQ_RX_DONE               (1 << 1)
#define SX1262_IRQ_TIMEOUT               (1 << 10)
#define SX1262_IRQ_ALL                   (SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_TIMEOUT)

/* SX1262 Status */
#define SX1262_STATUS_MODE_READY         0x02

/* LoRa Modulationsparameter */
#define LORA_CR_4_5                      0x01
#define LORA_CR_4_6                      0x02
#define LORA_CR_4_7                      0x03
#define LORA_CR_4_8                      0x04
#define LORA_CR_LI_4_5                   0x05
#define LORA_CR_LI_4_6                   0x06
#define LORA_CR_LI_4_7                   0x07
#define LORA_CR_LI_4_8                   0x08

/* Interne Variablen */
static spi_device_handle_t spi_handle = NULL;
static SemaphoreHandle_t lora_mutex = NULL;
static lora_rx_callback_t rx_callback = NULL;
static volatile lora_state_t current_state = LORA_STATE_IDLE;
static volatile int16_t last_rssi = 0;
static volatile int8_t last_snr = 0;
static TaskHandle_t dio1_task_handle = NULL;
static bool lora_initialized = false;
static uint8_t local_node_id = 1;
static volatile bool rx_enabled = false;

/* Zuletzt gesetzte TCXO-Spannungsstufe. Der SX1262 verwirft nach dem Reset oft
 * die ersten Kommandos, deshalb wird die TCXO-Konfiguration vor dem ersten
 * Empfang noch einmal gesetzt. */
static uint8_t s_tcxo_stufe = 0x02;   /* 0x02 = 1,8 V (Heltec Vorgabe) */

/* Messtest fuer den Oszillator: schaltet die DIO3-Versorgung im Sekundentakt
 * ein und aus. Am Vcc-Pad des 4-poligen Oszillators muss dann ein Rechteck
 * zwischen 0 V und etwa 1,8 V zu sehen sein. 0 = Test aus. */
#define LORA_TCXO_MESSTEST   20

/* Dauer einer Phase im Messtest. 200 ms passen zu 100 ms/Teil am Oszilloskop. */
#define LORA_MESSTEST_PHASE_MS  200

/* ====================================================================
 * SPI/GPI/O Hilfsfunktionen
 * ==================================================================== */

static esp_err_t wait_on_busy(uint32_t timeout_ms)
{
    uint64_t deadline = esp_timer_get_time() + (timeout_ms * 1000);
    while (gpio_get_level(LORA_BUSY_GPIO) == 1) {
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

/* Jeder Transfer benutzt getrennte Sende- und Empfangspuffer. Vorher zeigten
 * rx_buffer und tx_buffer auf denselben Speicher - dabei kamen nur Konstanten
 * wie 0xAA oder 0xA2 zurueck, also keine echten Chipdaten (das SyncWord liess
 * sich damit nicht zuruecklesen und der IRQ-Status war 0xAA00). */
static void spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(spi_handle, &t);
}

static uint8_t spi_xfer(uint8_t data)
{
    uint8_t tx = data;
    uint8_t rx = 0;
    spi_transfer(&tx, &rx, 1);
    return rx;
}

static void sx1262_cmd(uint8_t cmd)
{
    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(cmd);
    gpio_set_level(LORA_NSS_GPIO, 1);
}

static void sx1262_cmd_write_byte(uint8_t cmd, uint8_t data)
{
    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(cmd);
    spi_xfer(data);
    gpio_set_level(LORA_NSS_GPIO, 1);
}

static void sx1262_cmd_write_buf(uint8_t cmd, const uint8_t *data, size_t len)
{
    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(cmd);
    for (size_t i = 0; i < len; i++) {
        spi_xfer(data[i]);
    }
    gpio_set_level(LORA_NSS_GPIO, 1);
}

static void sx1262_cmd_read_buf(uint8_t cmd, uint8_t *buf, size_t len)
{
    /* Der SX1262 schiebt nach dem Kommando ZUERST sein Statusbyte heraus, erst
     * danach die Nutzdaten. Belegt durch Reg 0x0740: "A2 14 24 ..." - 0xA2 ist
     * der Status, 0x14 0x24 sind die geschriebenen Werte. Ohne dieses Byte
     * waren alle Antworten um eine Stelle verschoben; dadurch wurde TX-Done nie
     * erkannt und jede Aussendung lief in den Timeout. */
    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(cmd);
    (void)spi_xfer(0x00);           /* Statusbyte verwerfen */
    for (size_t i = 0; i < len; i++) {
        buf[i] = spi_xfer(0x00);
    }
    gpio_set_level(LORA_NSS_GPIO, 1);
}

/* Sendepuffer des SX1262 beschreiben. Offset und Daten gehoeren in EINEN
 * Zugriff; vorher stand ein 0x00 als eigenes Kommando davor, dadurch landeten
 * die Nutzdaten nie im Funkpuffer. */
static void sx1262_write_payload(uint8_t offset, const uint8_t *data, size_t len)
{
    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(SX1262_CMD_WRITE_BUFFER);
    spi_xfer(offset);
    for (size_t i = 0; i < len; i++) {
        spi_xfer(data[i]);
    }
    gpio_set_level(LORA_NSS_GPIO, 1);
}

/* Empfangspuffer des SX1262 lesen (Statusbyte zuerst, siehe oben). */
static void sx1262_read_payload(uint8_t offset, uint8_t *buf, size_t len)
{
    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(SX1262_CMD_READ_BUFFER);
    spi_xfer(offset);
    (void)spi_xfer(0x00);           /* Statusbyte verwerfen */
    for (size_t i = 0; i < len; i++) {
        buf[i] = spi_xfer(0x00);
    }
    gpio_set_level(LORA_NSS_GPIO, 1);
}

/* SetRx und SetTx brauchen drei Parameterbytes (Timeout in Schritten von
 * 15,625 ms). Ohne diese Bytes nimmt der SX1262 die Kommandos nicht an: er
 * sendet nicht und meldet auch keinen Abschluss - genau das erzeugte
 * "TX-Timeout nach 1000 ms". */
#define SX1262_RX_CONTINUOUS     0xFF, 0xFF, 0xFF
#define SX1262_TX_NO_TIMEOUT     0x00, 0x00, 0x00

static void sx1262_set_rx_continuous(void)
{
    uint8_t params[3] = { SX1262_RX_CONTINUOUS };
    sx1262_cmd_write_buf(SX1262_CMD_SET_RX, params, 3);
}

static void sx1262_set_tx(void)
{
    uint8_t params[3] = { SX1262_TX_NO_TIMEOUT };
    sx1262_cmd_write_buf(SX1262_CMD_SET_TX, params, 3);
}

static void sx1262_write_reg(uint16_t addr, uint8_t val)
{
    uint8_t buf[3] = { (addr >> 8) & 0xFF, addr & 0xFF, val };
    sx1262_cmd_write_buf(SX1262_CMD_WRITE_REGISTER, buf, 3);
}

/* Fehlerregister des SX1262 ausgeben. Wichtig: XOSC_START und PLL_LOCK zeigen
 * an, dass die Referenz bzw. die PLL nicht laeuft - dann werden SetTx und SetRx
 * abgelehnt, waehrend Registerzugriffe und Konfiguration weiter funktionieren. */
static void sx1262_log_device_errors(const char *wo)
{
    uint8_t err[2] = { 0, 0 };
    sx1262_cmd_read_buf(SX1262_CMD_GET_DEVICE_ERRORS, err, 2);
    uint16_t e = ((uint16_t)err[0] << 8) | err[1];

    ESP_LOGW(TAG, "Geraetefehler %s: 0x%04X%s%s%s%s%s", wo, e,
             (e & 0x0008) ? " ADC_CALIB" : "",
             (e & 0x0004) ? " PLL_CALIB" : "",
             (e & 0x0010) ? " IMG_CALIB" : "",
             (e & 0x0020) ? " XOSC_START" : "",
             (e & 0x0040) ? " PLL_LOCK" : "");
}

static uint8_t sx1262_read_reg(uint16_t addr)
{
    /* Register lesen geht in EINEM Zugriff: Kommando, Adresse, dann die Daten.
     * Zwei getrennte Zugriffe (wie vorher) brechen das Lesen ab, weil NSS
     * dazwischen hochgeht. */
    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(SX1262_CMD_READ_REGISTER);
    spi_xfer((addr >> 8) & 0xFF);
    spi_xfer(addr & 0xFF);
    (void)spi_xfer(0x00);           /* Statusbyte verwerfen */
    uint8_t result = spi_xfer(0x00);
    gpio_set_level(LORA_NSS_GPIO, 1);
    return result;
}

/* Diagnose: mehrere Bytes ab einer Registeradresse ausgeben. Damit laesst sich
 * sehen, ob und an welcher Stelle die geschriebenen Werte zurueckkommen. */
static void sx1262_dump_reg(uint16_t addr, int anzahl)
{
    char zeile[96];
    int pos = 0;

    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(SX1262_CMD_READ_REGISTER);
    spi_xfer((addr >> 8) & 0xFF);
    spi_xfer(addr & 0xFF);
    (void)spi_xfer(0x00);           /* Statusbyte verwerfen */
    for (int i = 0; i < anzahl; i++) {
        uint8_t v = spi_xfer(0x00);
        pos += snprintf(zeile + pos, sizeof(zeile) - pos, "%02X ", v);
    }
    gpio_set_level(LORA_NSS_GPIO, 1);

    ESP_LOGI(TAG, "Reg 0x%04X (%d Byte): %s", addr, anzahl, zeile);
}

/* ====================================================================
 * Interrupt-Handler (DIO1)
 * ==================================================================== */

static void IRAM_ATTR dio1_isr_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    if (dio1_task_handle) {
        vTaskNotifyGiveFromISR(dio1_task_handle, &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void dio1_event_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t irq[2];
        sx1262_cmd_read_buf(SX1262_CMD_GET_IRQSTATUS, irq, 2);
        uint16_t irq_mask = ((uint16_t)irq[0] << 8) | irq[1];
        sx1262_cmd_write_buf(SX1262_CMD_CLR_IRQSTATUS, irq, 2);

        if (irq_mask & SX1262_IRQ_TX_DONE) {
            ESP_LOGD(TAG, "TX abgeschlossen");

            /* Nach dem Senden wieder auf Empfang gehen. Ohne das hoert der Node
             * nach seinem ersten Paket nie wieder etwas: der SX1262 bleibt im
             * Standby stehen, bis ihn jemand neu auf RX setzt. */
            if (rx_enabled) {
                sx1262_set_rx_continuous();
                current_state = LORA_STATE_RX;
            } else {
                current_state = LORA_STATE_IDLE;
            }
        }

        if (irq_mask & SX1262_IRQ_RX_DONE) {
            ESP_LOGD(TAG, "RX empfangen");

            uint8_t buf_status[2];
            sx1262_cmd_read_buf(SX1262_CMD_GET_RXBUFFERSTATUS, buf_status, 2);
            uint8_t payload_len = buf_status[0];
            if (payload_len > LORA_MAX_PAYLOAD_LEN) {
                payload_len = LORA_MAX_PAYLOAD_LEN;
            }

            uint8_t pkt_status[3];
            sx1262_cmd_read_buf(SX1262_CMD_GET_PACKETSTATUS, pkt_status, 3);
            last_rssi = -pkt_status[0] / 2;
            last_snr = (int8_t)(pkt_status[1]) / 4;

            uint8_t raw[LORA_MAX_PAYLOAD_LEN];
            sx1262_read_payload(0x00, raw, payload_len);

            lora_message_t msg;
            memset(&msg, 0, sizeof(msg));

            if (payload_len >= 2) {
                msg.type = raw[0];
                msg.node_id = raw[1];
                msg.payload_len = (payload_len > 2) ? (payload_len - 2) : 0;
                if (msg.payload_len > 0) {
                    memcpy(msg.payload, raw + 2, msg.payload_len);
                }
            }
            msg.rssi = last_rssi;
            msg.snr = last_snr;

            current_state = LORA_STATE_IDLE;
            if (rx_callback) {
                rx_callback(&msg);
            }

            sx1262_set_rx_continuous();
        }

        if (irq_mask & SX1262_IRQ_TIMEOUT) {
            sx1262_set_rx_continuous();
        }
    }
}

/* ====================================================================
 * Oeffentliche API
 * ==================================================================== */

/* Vollstaendige Konfiguration des Funkteils. Wird beim Init und noch einmal
 * nach einem frischen Reset angewendet: manche Module uebernehmen die
 * Oszillator-Konfiguration erst dann. Enthaelt auch die DIO1-Maske, weil ein
 * Reset sie loeschen wuerde. */
static void sx1262_config_lora(void)
{
    sx1262_cmd_write_byte(SX1262_CMD_SET_STANDBY, 0x00);
    sx1262_cmd_write_byte(SX1262_CMD_SET_PACKETTYPE, 0x01);

    uint32_t frf = (uint32_t)((double)LORA_FREQUENCY / 15625.0);
    uint8_t rf[4] = { (frf >> 24) & 0xFF, (frf >> 16) & 0xFF,
                      (frf >> 8) & 0xFF, frf & 0xFF };
    sx1262_cmd_write_buf(SX1262_CMD_SET_RFFREQUENCY, rf, 4);

    /* Bild-Kalibrierung fuer 863-870 MHz (Kommando 0x98) und alle
     * Kalibrierbloecke (0x89). */
    uint8_t cal_img[2] = { 0xD7, 0xDB };
    sx1262_cmd_write_buf(SX1262_CMD_CALIBRATE_IMAGE, cal_img, 2);
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t cal_all = 0x7F;
    sx1262_cmd_write_buf(SX1262_CMD_CALIBRATE, &cal_all, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* PA-Konfiguration der SX1261 (Versionsregister meldet SX1261 V2D). */
    uint8_t pa_cfg[4] = { 0x04, 0x00, 0x01, 0x01 };
    sx1262_cmd_write_buf(SX1262_CMD_SET_PAOCONFIG, pa_cfg, 4);

    uint8_t txp[] = { LORA_TX_POWER, 0x02 };
    sx1262_cmd_write_buf(SX1262_CMD_SET_TXPARAMS, txp, 2);

    uint8_t modparams[] = { LORA_SF, 0x05, LORA_CR_4_5, 0x00 };  /* BW 125 kHz */
    sx1262_cmd_write_buf(SX1262_CMD_SET_LORAMODPARAMS, modparams, 4);

    uint8_t pktparams[] = { 0x00, LORA_PREAMBLE_LENGTH, 0x00,
                            LORA_MAX_PAYLOAD_LEN, 0x01, 0x00 };
    sx1262_cmd_write_buf(SX1262_CMD_SET_LORAPKTPARAMS, pktparams, 6);

    sx1262_write_reg(SX1262_REG_LORA_SYNCWORD, 0x14);
    sx1262_write_reg(SX1262_REG_LORA_SYNCWORD + 1, 0x24);

    /* DIO2 steuert den HF-Umschalter. */
    sx1262_cmd_write_byte(SX1262_CMD_SET_DIO2_AS_RF_SWITCH, 0x01);

    /* Regler auf DC-DC (0x02). Die Referenz setzt das ebenfalls; im LDO-Betrieb
     * bekommt die Sendeendstufe weniger Strom. */
    sx1262_cmd_write_byte(SX1262_CMD_SET_REGULATOR_MODE, 0x02);

    /* DIO1-Maske: global UND auf DIO1 (Reihenfolge: global, dio1, dio2, dio3). */
    uint8_t dio_irq[8] = {
        (SX1262_IRQ_ALL >> 8) & 0xFF, SX1262_IRQ_ALL & 0xFF,
        (SX1262_IRQ_ALL >> 8) & 0xFF, SX1262_IRQ_ALL & 0xFF,
        0x00, 0x00,
        0x00, 0x00,
    };
    sx1262_cmd_write_buf(SX1262_CMD_SET_DIOIRQPARAMS, dio_irq, 8);

    sx1262_cmd(SX1262_CMD_CLEAR_DEVICE_ERRORS);
    vTaskDelay(pdMS_TO_TICKS(10));
    sx1262_log_device_errors("nach der Konfiguration");
}

esp_err_t lora_init(void)
{
    if (lora_initialized) return ESP_OK;

    lora_mutex = xSemaphoreCreateMutex();
    if (!lora_mutex) return ESP_ERR_NO_MEM;

    /* GPIOs */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LORA_NSS_GPIO) | (1ULL << LORA_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(LORA_NSS_GPIO, 1);
    gpio_set_level(LORA_RST_GPIO, 1);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << LORA_BUSY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    gpio_config_t dio = {
        .pin_bit_mask = (1ULL << LORA_DIO1_GPIO),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&dio);

    /* SPI */
    spi_bus_config_t bus = {
        .mosi_io_num = LORA_MOSI_GPIO,
        .miso_io_num = LORA_MISO_GPIO,
        .sclk_io_num = LORA_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LORA_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        /* Testweise 2 MHz statt 8 MHz: prueft, ob der SX1262 wegen Timing oder
         * Signalqualitaet Kommandos verwirft (SetTx wurde nicht ausgefuehrt,
         * obwohl Konfigurationskommandos ankamen). 8 MHz sind laut Datenblatt
         * erlaubt, also waere ein Erfolg hier ein Hinweis auf die Verdrahtung. */
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LORA_SPI_HOST, &dev, &spi_handle));

    /* Reset */
    gpio_set_level(LORA_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (wait_on_busy(100) != ESP_OK) {
        ESP_LOGE(TAG, "Chip nach Reset nicht bereit");
        return ESP_ERR_TIMEOUT;
    }

#if LORA_TCXO_MESSTEST > 0
    /* Messung am Oszillator vorbereiten: ohne Konfiguration ist DIO3 aus
     * (0 V), mit SetDio3AsTcxoCtrl liegen 1,8 V an. Ein Reset loescht die
     * Konfiguration wieder, damit entsteht ein Rechteck fuer das Oszilloskop. */
    /* Dauerhafter Suchimpuls zum Auffinden des Pins: 200 ms DIO3 an (1,8 V),
     * 100 ms aus. Laeuft endlos, die restliche Anwendung startet dabei nicht.
     * Zum Abschalten LORA_TCXO_MESSTEST auf 0 setzen und neu flashen. */
    ESP_LOGW(TAG, "Suchimpuls aktiv: 200 ms an, 100 ms aus - dauerhaft");
    while (1) {
        uint8_t tcxo_puls[4] = { 0x02, 0x00, 0x06, 0x40 };
        sx1262_cmd_write_buf(SX1262_CMD_SET_DIO3_AS_TCXO_CTRL, tcxo_puls, 4);
        vTaskDelay(pdMS_TO_TICKS(200));

        /* Aus: ein Reset loescht die TCXO-Konfiguration */
        gpio_set_level(LORA_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(LORA_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(90));
    }
#endif

    /* Standby */
    sx1262_cmd_write_byte(SX1262_CMD_SET_STANDBY, 0x00);

    /* TCXO ueber DIO3 versorgen - Spannung suchen.
     * Gemessen (Build 56): mit 1,8 V stehen im Fehlerregister XOSC_START und
     * PLL_LOCK (0x0060). Die Referenz laeuft also nicht an, und deshalb lehnt
     * der Chip SetTx und SetRx ab, waehrend Registerzugriffe weiter gehen.
     * Hier werden die Stufen der Reihe nach gesetzt und jeweils Fehler gelesen;
     * 0x00 = 1,6 V ... 0x07 = 3,3 V. 0xFF = keine TCXO-Konfiguration (fuer den
     * Fall, dass das Board einen normalen Quarz hat). */
    const uint8_t tcxo_stufen[] = { 0xFF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };

    for (unsigned i = 0; i < sizeof(tcxo_stufen); i++) {
        if (tcxo_stufen[i] != 0xFF) {
            uint8_t tcxo_cfg[4] = { tcxo_stufen[i], 0x00, 0x06, 0x40 };
            sx1262_cmd_write_buf(SX1262_CMD_SET_DIO3_AS_TCXO_CTRL, tcxo_cfg, 4);
        }
        vTaskDelay(pdMS_TO_TICKS(60));

        uint8_t cal_stufe = 0x7F;
        sx1262_cmd_write_buf(SX1262_CMD_CALIBRATE, &cal_stufe, 1);
        vTaskDelay(pdMS_TO_TICKS(30));

        sx1262_cmd(SX1262_CMD_CLEAR_DEVICE_ERRORS);
        uint8_t err[2] = { 0, 0 };
        sx1262_cmd_read_buf(SX1262_CMD_GET_DEVICE_ERRORS, err, 2);
        uint16_t e = ((uint16_t)err[0] << 8) | err[1];

        ESP_LOGW(TAG, "TCXO-Stufe 0x%02X -> Geraetefehler 0x%04X", tcxo_stufen[i], e);
        if (e == 0) {
            ESP_LOGI(TAG, "TCXO-Einstellung gefunden: Spannung 0x%02X", tcxo_stufen[i]);
            s_tcxo_stufe = tcxo_stufen[i];
            break;
        }
    }

    /* Chip-Version auslesen (Register 0x0320, 16 Zeichen). Damit laesst sich
     * pruefen, welcher Chip wirklich auf dem Modul sitzt. */
    char version[17];
    for (int i = 0; i < 16; i++) {
        version[i] = (char)sx1262_read_reg(0x0320 + i);
    }
    version[16] = '\0';
    ESP_LOGI(TAG, "Chip-Version: %s", version);

    /* Komplette Konfiguration anwenden (inkl. Regler-Modus und DIO1-Maske). */
    sx1262_config_lora();

    /* Packet-Typ: LoRa */
    sx1262_cmd_write_byte(SX1262_CMD_SET_PACKETTYPE, 0x01);

    /* Frequenz: 868 MHz (Reg = freq / 15625) */
    uint32_t frf = (uint32_t)((double)LORA_FREQUENCY / 15625.0);
    uint8_t rf[4] = { (frf >> 24) & 0xFF, (frf >> 16) & 0xFF, (frf >> 8) & 0xFF, frf & 0xFF };
    sx1262_cmd_write_buf(SX1262_CMD_SET_RFFREQUENCY, rf, 4);

    /* Alle Kalibrierbloecke (0x89, Maske 0x7F) und danach die Bild-Kalibrierung
     * fuer das Band 863-870 MHz. ACHTUNG: Bild-Kalibrierung ist Kommando 0x98 -
     * 0x89 ist das allgemeine Calibrate mit EINEM Byte. Vorher stand hier 0x89
     * mit zwei Bytes, die noetige Bild-Kalibrierung fehlte also. */
    uint8_t cal_all = 0x7F;
    sx1262_cmd_write_buf(SX1262_CMD_CALIBRATE, &cal_all, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t cal_img[2] = { 0xD7, 0xDB };
    sx1262_cmd_write_buf(SX1262_CMD_CALIBRATE_IMAGE, cal_img, 2);

    /* BUSY muss jetzt kurz auf 1 gehen (die Kalibrierung dauert einige ms).
     * Bleibt die Leitung immer 0, ist sie nicht angeschlossen - dann werden alle
     * weiteren Kommandos ohne Ruecksicht auf den Chipzustand getaktet und der
     * Chip verwirft sie. */
    char busy_probe[16];
    int bp = 0;
    for (int i = 0; i < 10; i++) {
        bp += snprintf(busy_probe + bp, sizeof(busy_probe) - bp, "%d",
                       gpio_get_level(LORA_BUSY_GPIO));
    }
    ESP_LOGI(TAG, "BUSY direkt nach CalibrateImage: %s", busy_probe);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Kalibrierung ist durch - gemerkte Fehler loeschen und Stand ausgeben. */
    sx1262_cmd(SX1262_CMD_CLEAR_DEVICE_ERRORS);
    sx1262_log_device_errors("nach der Kalibrierung");

    /* PA-Konfiguration: SetPaConfig (0x95) braucht VIER Bytes:
     * paDutyCycle, hpMax, deviceSel, paLut.
     * WICHTIG: Das Versionsregister des Moduls meldet "SX1261 V2D" - also die
     * leistungsschwache Variante. Deren PA-Konfiguration ist
     * deviceSel = 0x01 und hpMax = 0x00 (max. 15 dBm). Mit den SX1262-Werten
     * (deviceSel = 0x00, 22 dBm) lehnt der Chip das Senden ab. */
    uint8_t pa_cfg[4] = { 0x04, 0x00, 0x01, 0x01 };
    sx1262_cmd_write_buf(SX1262_CMD_SET_PAOCONFIG, pa_cfg, 4);

    /* Sendeleistung und Rampe */
    uint8_t txp[] = { LORA_TX_POWER, 0x02 };
    sx1262_cmd_write_buf(SX1262_CMD_SET_TXPARAMS, txp, 2);

    sx1262_write_reg(SX1262_REG_OCP_CONFIG, 0x3B);
    sx1262_write_reg(SX1262_REG_TX_CLAMP_CURRENT, 0x08);

    /* LoRa-Modulationsparameter: SF, BW, CR */
    uint8_t bw;
    switch (LORA_BANDWIDTH) {
        case 0:  bw = 0x05; break; /* 125 kHz */
        case 1:  bw = 0x06; break; /* 250 kHz */
        case 2:  bw = 0x07; break; /* 500 kHz */
        default: bw = 0x05;
    }
    /* LDRO = 0: die Low-Data-Rate-Optimierung gilt nur fuer lange Symbole
     * (SF11/SF12 bei BW125). Bei SF7 muss sie aus sein, sonst versteht die
     * Gegenseite die Pakete nicht. */
    uint8_t modparams[] = { LORA_SF, bw, LORA_CR_4_5, 0x00 };
    sx1262_cmd_write_buf(SX1262_CMD_SET_LORAMODPARAMS, modparams, 4);

    /* Packet-Parameter: Preamble, Header, CRC, IQ */
    uint8_t pktparams[] = {
        (LORA_PREAMBLE_LENGTH >> 8) & 0xFF,
        LORA_PREAMBLE_LENGTH & 0xFF,
        0x00, /* Fixed/Explicit Header */
        LORA_MAX_PAYLOAD_LEN,
        0x01, /* CRC enabled */
        0x00, /* Standard IQ */
    };
    sx1262_cmd_write_buf(SX1262_CMD_SET_LORAPKTPARAMS, pktparams, 6);

    /* SyncWord: 0x1424 (privat, statt 0x3444 = oeffentlich) */
    sx1262_write_reg(SX1262_REG_LORA_SYNCWORD, 0x14);
    sx1262_write_reg(SX1262_REG_LORA_SYNCWORD + 1, 0x24);

    /* Detection-Optimierung fuer SF7 */
    sx1262_write_reg(SX1262_REG_LORA_DETECTION_OPT, 0x07);
    sx1262_write_reg(SX1262_REG_LORA_DETECTION_TH, 0x0A);

    /* Kontrolle: bekannte Register zuruecklesen. Erwartet werden die oben
     * geschriebenen Werte (SyncWord 0x14 0x24, Detection 0x07 0x0A). */
    sx1262_dump_reg(SX1262_REG_LORA_SYNCWORD, 6);
    sx1262_dump_reg(SX1262_REG_LORA_DETECTION_OPT, 3);
    ESP_LOGI(TAG, "Kontrolle: SyncWord gelesen 0x%02X 0x%02X (erwartet 0x14 0x24)",
             sx1262_read_reg(SX1262_REG_LORA_SYNCWORD),
             sx1262_read_reg(SX1262_REG_LORA_SYNCWORD + 1));

    /* DIO1-IRQ konfigurieren: TX Done, RX Done, Timeout.
     * Die 8 Bytes sind: globalIrqMask(2), dio1Mask(2), dio2Mask(2), dio3Mask(2).
     * Vorher stand die Maske im GLOBALEN Feld und dio1Mask blieb 0 - dadurch kam
     * am DIO1-Pin nie ein Interrupt an: jede Aussendung lief in den Timeout und
     * Empfang wurde nie gemeldet. */
    uint8_t dio_irq[8] = {
        (SX1262_IRQ_ALL >> 8) & 0xFF, SX1262_IRQ_ALL & 0xFF,
        (SX1262_IRQ_ALL >> 8) & 0xFF, SX1262_IRQ_ALL & 0xFF,
        0x00, 0x00,
        0x00, 0x00,
    };
    sx1262_cmd_write_buf(SX1262_CMD_SET_DIOIRQPARAMS, dio_irq, 8);

    /* DIO2 steuert den HF-Umschalter (im Schaltplan als SW_CT gezeichnet).
     * Ohne diese Einstellung wird die Sendeleistung nicht auf die Antenne
     * gefuehrt und der Empfaenger hoert nichts. */
    sx1262_cmd_write_byte(SX1262_CMD_SET_DIO2_AS_RF_SWITCH, 0x01);

    /* DIO1 Interrupt-Task starten */
    xTaskCreatePinnedToCore(dio1_event_task, "lora_dio1", 2048, NULL, 7, &dio1_task_handle, 1);

    /* GPIO-Interrupt fuer DIO1 */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(LORA_DIO1_GPIO, dio1_isr_handler, NULL);

    /* Node-ID aus NVS lesen (Default: 1) */
    extern uint8_t nvs_config_get_u8(const char *key, uint8_t default_val);
    local_node_id = 1; /* Wird in main.c gesetzt */

    lora_initialized = true;
    ESP_LOGI(TAG, "SX1262 initialisiert (%.3f MHz, SF=%d, BW=125kHz)",
             LORA_FREQUENCY / 1e6, LORA_SF);

    /* Selbsttest: eine kurze Aussendung noch waehrend der Initialisierung, also
     * bevor WiFi und die Tasks laufen. Klappt sie hier, ist der Funkweg in
     * Ordnung und ein spaeterer Ausfall liegt an der laufenden Anlage (z. B.
     * Versorgung). Klappt sie schon hier nicht, liegt es an der Konfiguration
     * des Chips. */
    /* Zweiter Anlauf mit frischem Reset: manche Module uebernehmen die
     * Oszillator-Konfiguration erst nach einem Reset, der auf das Setzen folgt.
     * Danach die komplette Konfiguration erneut anwenden (der Reset loescht sie
     * samt DIO1-Maske). */
    gpio_set_level(LORA_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    if (wait_on_busy(100) != ESP_OK) {
        ESP_LOGW(TAG, "Chip nach dem zweiten Reset nicht bereit");
    }
    uint8_t tcxo_zweit[4] = { s_tcxo_stufe, 0x00, 0x06, 0x40 };
    sx1262_cmd_write_buf(SX1262_CMD_SET_DIO3_AS_TCXO_CTRL, tcxo_zweit, 4);
    vTaskDelay(pdMS_TO_TICKS(50));
    sx1262_config_lora();

    lora_message_t test_msg;
    memset(&test_msg, 0, sizeof(test_msg));
    test_msg.type = LORA_MSG_TYPE_STATUS;
    test_msg.node_id = local_node_id;
    test_msg.payload_len = 1;
    test_msg.payload[0] = 0x5A;

    rx_enabled = true;
    esp_err_t test_ret = lora_send(&test_msg, 1000);
    ESP_LOGW(TAG, "Selbsttest Senden: %s", esp_err_to_name(test_ret));
    sx1262_log_device_errors("nach dem Selbsttest");

    return ESP_OK;
}

esp_err_t lora_deinit(void)
{
    if (!lora_initialized) return ESP_OK;

    sx1262_cmd_write_byte(SX1262_CMD_SET_SLEEP, 0x00);

    if (dio1_task_handle) {
        vTaskDelete(dio1_task_handle);
        dio1_task_handle = NULL;
    }

    gpio_isr_handler_remove(LORA_DIO1_GPIO);

    if (spi_handle) {
        spi_bus_remove_device(spi_handle);
        spi_handle = NULL;
    }
    spi_bus_free(LORA_SPI_HOST);

    if (lora_mutex) {
        vSemaphoreDelete(lora_mutex);
        lora_mutex = NULL;
    }

    lora_initialized = false;
    current_state = LORA_STATE_IDLE;
    ESP_LOGI(TAG, "SX1262 deinitialisiert");
    return ESP_OK;
}

esp_err_t lora_send(const lora_message_t *msg, uint32_t timeout_ms)
{
    if (!lora_initialized || !msg) return ESP_ERR_INVALID_ARG;
    if (timeout_ms == 0) timeout_ms = 5000;

    xSemaphoreTake(lora_mutex, portMAX_DELAY);

    current_state = LORA_STATE_TX;

    /* Payload aufbauen: [type, node_id, payload...] */
    uint8_t raw[LORA_MAX_PAYLOAD_LEN];
    size_t raw_len = 2 + msg->payload_len;
    if (raw_len > LORA_MAX_PAYLOAD_LEN) raw_len = LORA_MAX_PAYLOAD_LEN;

    raw[0] = msg->type;
    raw[1] = msg->node_id;
    if (msg->payload_len > 0) {
        memcpy(raw + 2, msg->payload, msg->payload_len);
    }

    /* Write Buffer */
    raw_len = (raw_len > LORA_MAX_PAYLOAD_LEN) ? LORA_MAX_PAYLOAD_LEN : raw_len;
    sx1262_write_payload(0x00, raw, raw_len);

    /* Payload-Laenge setzen */
    uint8_t pktparams[] = {
        (LORA_PREAMBLE_LENGTH >> 8) & 0xFF,
        LORA_PREAMBLE_LENGTH & 0xFF,
        0x00,
        raw_len,
        0x01,
        0x00,
    };
    sx1262_cmd_write_buf(SX1262_CMD_SET_LORAPKTPARAMS, pktparams, 6);

    /* Senden */
    sx1262_set_tx();
    ESP_LOGD(TAG, "Sende %u Bytes", raw_len);

    /* Diagnose: den Chipmodus direkt nach dem Start verfolgen (Modus 6 = TX,
     * 5 = RX, 3 = Standby-XOSC). Nach 300 ms waere ein SendeVorgang mit SF7 und
     * wenigen Byte laengst vorbei, deshalb im 5-ms-Raster messen. */
    char moden[80];
    int mp = 0;
    for (int i = 0; i < 14; i++) {
        uint8_t st_dbg = 0;
        sx1262_cmd_read_buf(0xC0, &st_dbg, 1);
        mp += snprintf(moden + mp, sizeof(moden) - mp, "%X", (st_dbg >> 4) & 0x07);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "Chipmodus nach SET_TX (5-ms-Raster): %s", moden);

    xSemaphoreGive(lora_mutex);

    /* Auf TX-Done warten (nicht-blockierend bis Timeout) */
    TickType_t start = xTaskGetTickCount();
    while (current_state == LORA_STATE_TX) {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            /* Diagnose: hat der Chip den Abschluss gemeldet? Dann liegt es am
             * Interrupt-Weg. Ist der IRQ-Status 0, hat er gar nicht gesendet.
             * Das Statusbyte zeigt zusaetzlich den Chipmodus (2 = Standby,
             * 5 = RX, 6 = TX). */
            uint8_t irq[2] = { 0, 0 };
            uint8_t status = 0;
            sx1262_cmd_read_buf(SX1262_CMD_GET_IRQSTATUS, irq, 2);
            sx1262_cmd_read_buf(0xC0, &status, 1);   /* GetStatus */
            ESP_LOGW(TAG, "TX-Timeout nach %lu ms (IRQ 0x%02X%02X, Status 0x%02X, DIO1=%d)",
                     (unsigned long)timeout_ms, irq[0], irq[1], status,
                     gpio_get_level(LORA_DIO1_GPIO));
            sx1262_log_device_errors("beim Senden");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}

esp_err_t lora_send_async(const lora_message_t *msg)
{
    if (!lora_initialized || !msg) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(lora_mutex, portMAX_DELAY);

    current_state = LORA_STATE_TX;

    uint8_t raw[LORA_MAX_PAYLOAD_LEN];
    size_t raw_len = 2 + msg->payload_len;
    if (raw_len > LORA_MAX_PAYLOAD_LEN) raw_len = LORA_MAX_PAYLOAD_LEN;

    raw[0] = msg->type;
    raw[1] = msg->node_id;
    if (msg->payload_len > 0) {
        memcpy(raw + 2, msg->payload, msg->payload_len);
    }

    sx1262_write_payload(0x00, raw, raw_len);

    uint8_t pktparams[] = {
        (LORA_PREAMBLE_LENGTH >> 8) & 0xFF,
        LORA_PREAMBLE_LENGTH & 0xFF,
        0x00, raw_len, 0x01, 0x00,
    };
    sx1262_cmd_write_buf(SX1262_CMD_SET_LORAPKTPARAMS, pktparams, 6);

    sx1262_set_tx();

    xSemaphoreGive(lora_mutex);
    return ESP_OK;
}

esp_err_t lora_start_rx(void)
{
    if (!lora_initialized) return ESP_ERR_INVALID_STATE;

    /* TCXO hier erneut setzen: nach dem Reset verwirft der SX1262 oft die ersten
     * Kommandos. Bleibt die Referenz aus, meldet er XOSC_START und lehnt RX/TX
     * ab. Deshalb vor dem ersten Empfang nochmal setzen und pruefen. */
    uint8_t tcxo_cfg[4] = { s_tcxo_stufe, 0x00, 0x06, 0x40 };
    sx1262_cmd_write_buf(SX1262_CMD_SET_DIO3_AS_TCXO_CTRL, tcxo_cfg, 4);
    vTaskDelay(pdMS_TO_TICKS(50));
    sx1262_cmd(SX1262_CMD_CLEAR_DEVICE_ERRORS);
    vTaskDelay(pdMS_TO_TICKS(20));
    sx1262_log_device_errors("vor dem Empfang");

    /* Gegenprobe nach dem WiFi-Start: antwortet der Chip noch mit echten Daten?
     * Ein Wert wie 0xB2B2 bedeutet, dass er nur noch sein Statusbyte ausgibt -
     * dann stimmt etwas mit Versorgung oder Bustaktung nicht. */
    ESP_LOGI(TAG, "Gegenprobe: SyncWord 0x%02X 0x%02X (erwartet 0x14 0x24)",
             sx1262_read_reg(SX1262_REG_LORA_SYNCWORD),
             sx1262_read_reg(SX1262_REG_LORA_SYNCWORD + 1));

    /* Paket-Laenge auf Maximum setzen */
    uint8_t pktparams[] = {
        (LORA_PREAMBLE_LENGTH >> 8) & 0xFF,
        LORA_PREAMBLE_LENGTH & 0xFF,
        0x00,
        LORA_MAX_PAYLOAD_LEN,
        0x01,
        0x00,
    };
    sx1262_cmd_write_buf(SX1262_CMD_SET_LORAPKTPARAMS, pktparams, 6);

    current_state = LORA_STATE_RX;
    rx_enabled = true;
    sx1262_set_rx_continuous();
    ESP_LOGI(TAG, "RX-Modus gestartet");
    return ESP_OK;
}

esp_err_t lora_stop_rx(void)
{
    if (!lora_initialized) return ESP_ERR_INVALID_STATE;

    sx1262_cmd_write_byte(SX1262_CMD_SET_STANDBY, 0x00);
    rx_enabled = false;
    current_state = LORA_STATE_IDLE;
    ESP_LOGI(TAG, "RX-Modus gestoppt");
    return ESP_OK;
}

esp_err_t lora_set_rx_callback(lora_rx_callback_t callback)
{
    rx_callback = callback;
    return ESP_OK;
}

lora_state_t lora_get_state(void)
{
    return current_state;
}

int16_t lora_get_last_rssi(void)
{
    return last_rssi;
}

int8_t lora_get_last_snr(void)
{
    return last_snr;
}

int16_t lora_get_temperature(void)
{
    if (!lora_initialized) return 0;

    /* Temperatur aus SX1262 Register lesen (0x00 bis 0xFF -> -127 bis +128 °C) */
    uint8_t raw = sx1262_read_reg(0x00);
    return (int16_t)((int8_t)raw) * 10;
}

esp_err_t lora_ping(uint8_t target_node_id, uint32_t timeout_ms)
{
    if (!lora_initialized) return ESP_ERR_INVALID_STATE;
    if (timeout_ms == 0) timeout_ms = 5000;

    lora_message_t ping_msg;
    memset(&ping_msg, 0, sizeof(ping_msg));
    ping_msg.type = LORA_MSG_TYPE_PING;
    ping_msg.node_id = local_node_id;
    ping_msg.payload[0] = target_node_id;
    ping_msg.payload_len = 1;

    return lora_send(&ping_msg, timeout_ms);
}

void lora_set_node_id(uint8_t node_id)
{
    local_node_id = node_id;
}

void lora_debug_check(const char *wo)
{
    if (!lora_initialized) {
        return;
    }

    /* Versionsregister lesen: kommt "SX12" heraus, antwortet der Chip auf
     * Kommandos. Kommen nur Statusbytes (0xB2), ist er ausgestiegen. */
    char version[5];
    for (int i = 0; i < 4; i++) {
        version[i] = (char)sx1262_read_reg(0x0320 + i);
    }
    version[4] = '\0';

    uint8_t err[2] = { 0, 0 };
    sx1262_cmd_read_buf(SX1262_CMD_GET_DEVICE_ERRORS, err, 2);

    ESP_LOGI(TAG, "Pruefung %s: Version '%s', Fehler 0x%02X%02X",
             wo, version, err[0], err[1]);
}

uint8_t lora_get_node_id(void)
{
    return local_node_id;
}