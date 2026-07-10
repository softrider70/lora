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

static void spi_write(uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_transmit(spi_handle, &t);
}

static void spi_read(uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .rx_buffer = data,
        .tx_buffer = data,
    };
    spi_device_transmit(spi_handle, &t);
}

static uint8_t spi_xfer(uint8_t data)
{
    uint8_t buf = data;
    spi_read(&buf, 1);
    return buf;
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
    wait_on_busy(100);
    gpio_set_level(LORA_NSS_GPIO, 0);
    spi_xfer(cmd);
    for (size_t i = 0; i < len; i++) {
        buf[i] = spi_xfer(0x00);
    }
    gpio_set_level(LORA_NSS_GPIO, 1);
}

static void sx1262_write_reg(uint16_t addr, uint8_t val)
{
    uint8_t buf[3] = { (addr >> 8) & 0xFF, addr & 0xFF, val };
    sx1262_cmd_write_buf(SX1262_CMD_WRITE_REGISTER, buf, 3);
}

static uint8_t sx1262_read_reg(uint16_t addr)
{
    uint8_t buf[2] = { (addr >> 8) & 0xFF, addr & 0xFF };
    sx1262_cmd_write_buf(SX1262_CMD_READ_REGISTER, buf, 2);
    uint8_t result;
    sx1262_cmd_read_buf(0x00, &result, 1);
    return result;
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
            current_state = LORA_STATE_IDLE;
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
            uint8_t read_cmd[2] = { 0, 0 };
            sx1262_cmd_write_buf(SX1262_CMD_READ_BUFFER, read_cmd, 2);
            sx1262_cmd_read_buf(0x00, raw, payload_len);

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

            sx1262_cmd(SX1262_CMD_SET_RX);
        }

        if (irq_mask & SX1262_IRQ_TIMEOUT) {
            sx1262_cmd(SX1262_CMD_SET_RX);
        }
    }
}

/* ====================================================================
 * Oeffentliche API
 * ==================================================================== */

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
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
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

    /* Standby */
    sx1262_cmd_write_byte(SX1262_CMD_SET_STANDBY, 0x00);

    /* Packet-Typ: LoRa */
    sx1262_cmd_write_byte(SX1262_CMD_SET_PACKETTYPE, 0x01);

    /* Frequenz: 868 MHz (Reg = freq / 15625) */
    uint32_t frf = (uint32_t)((double)LORA_FREQUENCY / 15625.0);
    uint8_t rf[4] = { (frf >> 24) & 0xFF, (frf >> 16) & 0xFF, (frf >> 8) & 0xFF, frf & 0xFF };
    sx1262_cmd_write_buf(SX1262_CMD_SET_RFFREQUENCY, rf, 4);

    /* PA-Konfiguration */
    sx1262_cmd_write_byte(SX1262_CMD_SET_PAOCONFIG, 0x04);
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
    uint8_t modparams[] = { LORA_SF, bw, LORA_CR_4_5, 0x01 };
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

    /* DIO1-IRQ konfigurieren: TX Done, RX Done, Timeout */
    uint8_t dio_irq[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    dio_irq[0] = (SX1262_IRQ_ALL >> 8) & 0xFF;
    dio_irq[1] = SX1262_IRQ_ALL & 0xFF;
    sx1262_cmd_write_buf(SX1262_CMD_SET_DIOIRQPARAMS, dio_irq, 8);

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
    uint8_t buf_len[2] = { 0, 0 };
    sx1262_cmd_write_buf(SX1262_CMD_WRITE_BUFFER, buf_len, 2);
    sx1262_cmd_write_buf(0x00, raw, raw_len);

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
    sx1262_cmd(SX1262_CMD_SET_TX);
    ESP_LOGD(TAG, "Sende %u Bytes", raw_len);

    xSemaphoreGive(lora_mutex);

    /* Auf TX-Done warten (nicht-blockierend bis Timeout) */
    TickType_t start = xTaskGetTickCount();
    while (current_state == LORA_STATE_TX) {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGW(TAG, "TX-Timeout nach %lu ms", timeout_ms);
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

    uint8_t buf_len[2] = { 0, 0 };
    sx1262_cmd_write_buf(SX1262_CMD_WRITE_BUFFER, buf_len, 2);
    sx1262_cmd_write_buf(0x00, raw, raw_len);

    uint8_t pktparams[] = {
        (LORA_PREAMBLE_LENGTH >> 8) & 0xFF,
        LORA_PREAMBLE_LENGTH & 0xFF,
        0x00, raw_len, 0x01, 0x00,
    };
    sx1262_cmd_write_buf(SX1262_CMD_SET_LORAPKTPARAMS, pktparams, 6);

    sx1262_cmd(SX1262_CMD_SET_TX);

    xSemaphoreGive(lora_mutex);
    return ESP_OK;
}

esp_err_t lora_start_rx(void)
{
    if (!lora_initialized) return ESP_ERR_INVALID_STATE;

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
    sx1262_cmd(SX1262_CMD_SET_RX);
    ESP_LOGI(TAG, "RX-Modus gestartet");
    return ESP_OK;
}

esp_err_t lora_stop_rx(void)
{
    if (!lora_initialized) return ESP_ERR_INVALID_STATE;

    sx1262_cmd_write_byte(SX1262_CMD_SET_STANDBY, 0x00);
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

uint8_t lora_get_node_id(void)
{
    return local_node_id;
}