/*
 * display.c - SSD1306 OLED Display Treiber fuer Heltec WiFi LoRa 32 V3
 *
 * I2C-Konfiguration: SDA=GPIO41, SCL=GPIO42, Addr=0x3C, 128x64 Pixel
 * Verwendet einen 1024-Byte Framebuffer (128x64 / 8 Bitplanes).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "config.h"
#include "display.h"
#include "lora.h"

static const char *TAG = "DISPLAY";

/* Display-Dimensionen */
#define DISPLAY_PAGES       (DISPLAY_HEIGHT / 8)    /* 8 Seiten zu je 8 Pixeln */

/* SSD1306 Kommandos */
#define SSD1306_CMD_SETCONTRAST           0x81
#define SSD1306_CMD_DISPLAYALLON_RESUME   0xA4
#define SSD1306_CMD_DISPLAYALLON          0xA5
#define SSD1306_CMD_NORMALDISPLAY         0xA6
#define SSD1306_CMD_INVERTDISPLAY         0xA7
#define SSD1306_CMD_DISPLAYOFF            0xAE
#define SSD1306_CMD_DISPLAYON             0xAF
#define SSD1306_CMD_SETDISPLAYOFFSET      0xD3
#define SSD1306_CMD_SETCOMPINS            0xDA
#define SSD1306_CMD_SETVCOMDETECT         0xDB
#define SSD1306_CMD_SETDISPLAYCLOCKDIV    0xD5
#define SSD1306_CMD_SETPRECHARGE          0xD9
#define SSD1306_CMD_SETMULTIPLEX          0xA8
#define SSD1306_CMD_SETLOWCOLUMN          0x00
#define SSD1306_CMD_SETHIGHCOLUMN         0x10
#define SSD1306_CMD_SETSTARTLINE          0x40
#define SSD1306_CMD_MEMORYMODE            0x20
#define SSD1306_CMD_COLUMNADDR            0x21
#define SSD1306_CMD_PAGEADDR              0x22
#define SSD1306_CMD_SEGREMAP              0xA0
#define SSD1306_CMD_COMSCANDEC            0xC8
#define SSD1306_CMD_CHARGEPUMP            0x8D
#define SSD1306_CMD_ACTIVATE_SCROLL       0x2F
#define SSD1306_CMD_DEACTIVATE_SCROLL     0x2E
#define SSD1306_CMD_SET_VERTICAL_SCROLL   0xA3

/* 5x7 Font (ASCII 0x20-0x7F) */
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* Space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x01,0x01}, /* F */
    {0x3E,0x41,0x41,0x51,0x32}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x00,0x7F,0x41,0x41}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* Backslash */
    {0x41,0x41,0x7F,0x00,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x08,0x14,0x54,0x54,0x3C}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x00,0x7F,0x10,0x28,0x44}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
};

static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t dev_handle = NULL;
static uint8_t framebuffer[DISPLAY_PAGES][DISPLAY_WIDTH];
static bool display_initialized = false;

/* display_update() wird schon in display_init() fuer das Testbild gebraucht,
 * ist aber weiter unten definiert. */
static void display_update(void);

/* Transfer mit Wiederholung - OHNE Bus-Reset. Messungen (Build 150/151):
 * der SSD1306 laesst einzelne Transfers unbeantwortet (ESP_ERR_INVALID_RESPONSE),
 * dabei bleibt SDA kurz tief und ist 10 ms spaeter wieder frei. Ein Reset des
 * Busses machte daraus eine Lawine (74 Fehler/min) - eine reine Wiederholung
 * nach kurzer Pause ist richtig. */
static esp_err_t display_i2c_transfer(const uint8_t *buf, size_t len, int versuche)
{
    esp_err_t ret = ESP_FAIL;
    static uint32_t ok = 0, aussetzer = 0, fehl = 0;

    for (int versuch = 1; versuch <= versuche; versuch++) {
        ret = i2c_master_transmit(dev_handle, buf, len, pdMS_TO_TICKS(100));
        if (ret == ESP_OK) {
            ok++;
            if (versuch > 1) {
                aussetzer++;
            }
            break;
        }
        if (versuch < versuche) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    if (ret != ESP_OK) {
        fehl++;
        ESP_LOGE(TAG, "Transfer fehlgeschlagen: %d Bytes, %s",
                 (int)len, esp_err_to_name(ret));
    }

    /* Einmal je Minute eine Statuszeile: zeigt, ob ueberhaupt Transfers laufen
     * und wie viele Aussetzer es gibt. */
    static TickType_t letzte_ausgabe = 0;
    TickType_t jetzt = xTaskGetTickCount();
    if (letzte_ausgabe == 0 || (jetzt - letzte_ausgabe) > pdMS_TO_TICKS(60000)) {
        letzte_ausgabe = jetzt;
        ESP_LOGI(TAG, "Transfers ok: %lu, davon wiederholt: %lu, fehlgeschlagen: %lu",
                 (unsigned long)ok, (unsigned long)aussetzer, (unsigned long)fehl);
    }

    return ret;
}

/* SSD1306 Kommando senden. Kommandos sind idempotent, sie duerfen wiederholt
 * werden - noetig, weil der SSD1306 direkt nach dem Reset einzelne Transfers
 * unbeantwortet laesst. */
static esp_err_t display_send_cmd(uint8_t cmd)
{
    uint8_t data[2] = { 0x00, cmd }; /* Co=0, D/C#=0 -> Command */
    return display_i2c_transfer(data, 2, 3);
}

/* SSD1306 Daten senden (framebuffer pageweise). Bewusst OHNE Wiederholung:
 * der SSD1306 zaehlt im Horizontal-Mode nach 128 Bytes weiter und wuerde bei
 * einem zweiten Anlauf die folgenden Seiten verschieben. */
static esp_err_t display_send_data(uint8_t page, const uint8_t *data, size_t len)
{
    uint8_t *buf = malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    buf[0] = 0x40; /* Co=0, D/C#=1 -> Data */
    memcpy(buf + 1, data, len);

    esp_err_t ret = display_i2c_transfer(buf, len + 1, 1);
    free(buf);
    return ret;
}

/* SSD1306 initialisieren (Standard-Sequenz) */
static esp_err_t ssd1306_init_seq(void)
{
    esp_err_t ret;

    /* Init-Sequenz */
    ret = display_send_cmd(SSD1306_CMD_DISPLAYOFF);
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SETDISPLAYCLOCKDIV);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0x80); /* Default: 0x80 */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SETMULTIPLEX);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0x3F); /* 64 MUX */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SETDISPLAYOFFSET);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0x00); /* No offset */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SETSTARTLINE | 0x00);
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_CHARGEPUMP);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0x14); /* Enable charge pump */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_MEMORYMODE);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0x00); /* Horizontal addressing */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SEGREMAP | 0x01); /* Column 127 = SEG0 */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_COMSCANDEC); /* COM63 to COM0 */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SETCOMPINS);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0x12); /* Alternative pins */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SETCONTRAST);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0x7F); /* Mittelwert */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SETPRECHARGE);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0xF1); /* Phase 1: 15, Phase 2: 1 */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_SETVCOMDETECT);
    if (ret != ESP_OK) return ret;
    ret = display_send_cmd(0x40); /* Vcom = 0.77 * Vcc */
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_DISPLAYALLON_RESUME);
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_NORMALDISPLAY);
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_DEACTIVATE_SCROLL);
    if (ret != ESP_OK) return ret;

    ret = display_send_cmd(SSD1306_CMD_DISPLAYON);
    return ret;
}

/* Diagnose: Auf welchen Adressen antwortet ein Chip an diesem Pin-Paar?
 * Wird nur aufgerufen, wenn die Display-Initialisierung fehlschlaegt. So steht
 * im Log, ob es an den Pins, an der Adresse oder an der Versorgung liegt. */
static void display_i2c_scan_pins(int sda, int scl)
{
    i2c_master_bus_handle_t scan_bus = NULL;
    i2c_master_bus_config_t scan_cfg = {
        .i2c_port = I2C_NUM_1,      /* Nummer 0 ist vom Display belegt */
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    if (i2c_new_master_bus(&scan_cfg, &scan_bus) != ESP_OK) {
        ESP_LOGW(TAG, "Scan auf SDA=%d/SCL=%d nicht moeglich", sda, scl);
        return;
    }

    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(scan_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  Antwort 0x%02X (SDA=%d, SCL=%d)", addr, sda, scl);
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "  Kein Chip auf SDA=%d, SCL=%d", sda, scl);
    }

    i2c_del_master_bus(scan_bus);

    /* Ruhepegel der beiden Leitungen: 1/1 = Bus frei, es fehlt nur ein Geraet.
     * 0 = Leitung wird festgehalten, dann stimmt etwas an der Beschaltung nicht. */
    gpio_config_t pegel = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pegel);
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_LOGI(TAG, "  Ruhepegel SDA=%d SCL=%d (1 = hoch/frei)",
             gpio_get_level(sda), gpio_get_level(scl));
}

/* Display initialisieren */
esp_err_t display_init(void)
{
    esp_err_t ret;

    if (display_initialized) {
        return ESP_OK;
    }

    /* Vext einschalten - erst damit ist das OLED versorgt (LOW = ein).
     * Ohne Vext antwortet der SSD1306 nicht auf der I2C-Adresse. */
    gpio_config_t vext = {
        .pin_bit_mask = (1ULL << DISPLAY_VEXT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vext);
    gpio_set_level(DISPLAY_VEXT_GPIO, 0);
    ESP_LOGI(TAG, "Vext (GPIO%d) eingeschaltet", DISPLAY_VEXT_GPIO);
    /* 200 ms statt 50 ms: Vext braucht beim Hochlaufen Zeit. Mit 50 ms liefen
     * die ersten Transfers nach dem Init in den Timeout (im Log zu sehen). */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* OLED-Reset freigeben. Bleibt RST offen, haelt der SSD1306 den Reset und
     * antwortet nicht auf seiner I2C-Adresse (ESP_ERR_INVALID_RESPONSE). */
    gpio_config_t rst = {
        .pin_bit_mask = (1ULL << DISPLAY_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);
    gpio_set_level(DISPLAY_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(DISPLAY_RST_GPIO, 1);
    ESP_LOGI(TAG, "OLED-Reset (GPIO%d) freigegeben", DISPLAY_RST_GPIO);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Initialisiere I2C (SDA=%d, SCL=%d, Freq=%d Hz)",
             DISPLAY_SDA_GPIO, DISPLAY_SCL_GPIO, DISPLAY_I2C_FREQ);

    /* I2C-Bus konfigurieren */
    i2c_master_bus_config_t bus_config = {
        .i2c_port = DISPLAY_I2C_PORT,
        .sda_io_num = DISPLAY_SDA_GPIO,
        .scl_io_num = DISPLAY_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ret = i2c_new_master_bus(&bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C-Bus-Init fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    /* I2C-Device konfigurieren */
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DISPLAY_I2C_ADDR,
        .scl_speed_hz = DISPLAY_I2C_FREQ,
    };

    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C-Device-Add fehlgeschlagen: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        return ret;
    }

    /* Warten, bis das OLED auf seiner Adresse antwortet. Direkt nach dem Reset
     * nimmt der SSD1306 noch keine Kommandos an: im Log scheiterte der erste
     * Transfer mit ESP_ERR_INVALID_RESPONSE, waehrend ein Probe 20 ms spaeter
     * auf derselben Adresse ein ACK bekam. */
    int warte_ms = 0;
    while (warte_ms < 1000) {
        if (i2c_master_probe(bus_handle, DISPLAY_I2C_ADDR, 50) == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        warte_ms += 50;
    }
    ESP_LOGI(TAG, "OLED antwortet nach %d ms Wartezeit", warte_ms);

    /* SSD1306 initialisieren */
    ret = ssd1306_init_seq();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306-Init fehlgeschlagen: %s", esp_err_to_name(ret));

        /* Diagnose: wer antwortet auf welchem Pin-Paar? */
        ESP_LOGW(TAG, "I2C-Scan auf den konfigurierten Pins:");
        display_i2c_scan_pins(DISPLAY_SDA_GPIO, DISPLAY_SCL_GPIO);
        ESP_LOGW(TAG, "I2C-Scan auf den alten Pins (Gegenprobe):");
        display_i2c_scan_pins(DISPLAY_ALT_SDA_GPIO, DISPLAY_ALT_SCL_GPIO);

        i2c_master_bus_rm_device(dev_handle);
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        dev_handle = NULL;
        return ret;
    }

    /* WICHTIG: Das Flag muss VOR dem ersten Schreiben gesetzt werden.
     * display_update(), display_clear() und display_update_page() steigen bei
     * !display_initialized sofort wieder aus. Stand das Flag erst danach,
     * wurde der Bildspeicher nie geloescht: das Panel zeigte den zufaelligen
     * Einschaltinhalt des SSD1306 - ein weisses Rauschbild. */
    display_initialized = true;

    /* Bildspeicher loeschen und senden. In diesem Moment laeuft die
     * LoRa-Initialisierung mit ihren Stromspitzen; einzelne Seiten kommen
     * deshalb nicht an (im Log: "Transfer fehlgeschlagen: 129 Bytes"). Das ist
     * unkritisch: der Display-Task schreibt alle acht Zeilen im Sekundentakt
     * neu, sobald der Funk ruhig ist. */
    memset(framebuffer, 0, sizeof(framebuffer));
    display_update();

    ESP_LOGI(TAG, "Display initialisiert (128x64, I2C Addr 0x%02X)", DISPLAY_I2C_ADDR);
    return ESP_OK;
}

/* Display deinitialisieren */
esp_err_t display_deinit(void)
{
    if (!display_initialized) {
        return ESP_OK;
    }

    /* Display ausschalten */
    display_send_cmd(SSD1306_CMD_DISPLAYOFF);

    /* I2C-Ressourcen freigeben */
    if (dev_handle) {
        i2c_master_bus_rm_device(dev_handle);
        dev_handle = NULL;
    }
    if (bus_handle) {
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
    }

    display_initialized = false;
    ESP_LOGI(TAG, "Display deinitialisiert");
    return ESP_OK;
}

/* Eine Seite (8 Pixelzeilen, 128 Byte) uebertragen - mit Wiederholung.
 * Vor JEDEM Versuch wird die Adresse neu gesetzt. Damit ist der zweite Anlauf
 * gefahrlos: bricht ein Transfer mitten in der Datenfolge ab, steht der
 * SSD1306 im Horizontal-Mode hinter der letzten geschriebenen Spalte - die
 * Adresse holt ihn zurueck an den Seitenanfang. Ohne dieses Zuruecksetzen
 * wandern die Daten in die naechste Seite. */
static void display_send_page(int page)
{
    for (int versuch = 1; versuch <= 3; versuch++) {
        display_send_cmd(SSD1306_CMD_COLUMNADDR);
        display_send_cmd(0);
        display_send_cmd(DISPLAY_WIDTH - 1);
        display_send_cmd(SSD1306_CMD_PAGEADDR);
        display_send_cmd(page);
        display_send_cmd(page);

        if (display_send_data(page, framebuffer[page], DISPLAY_WIDTH) == ESP_OK) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* Framebuffer aufs Display uebertragen */
static void display_update(void)
{
    if (!display_initialized) return;

    for (int page = 0; page < DISPLAY_PAGES; page++) {
        display_send_page(page);
    }
}

/* Eine einzelne Seite (8 Pixelzeilen) uebertragen. Beim Zeilenzeichnen wird
 * nur die betroffene Seite gesendet: das spart I2C-Verkehr und verhindert, dass
 * bei einem Uebertragungsfehler andere Zeilen leer bleiben. */
static void display_update_page(int page)
{
    if (!display_initialized) return;
    if (page < 0 || page >= DISPLAY_PAGES) return;

    display_send_page(page);
}

/* Ein Pixel im Framebuffer setzen */
static void display_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) return;

    int page = y / 8;
    int bit = y % 8;

    if (on) {
        framebuffer[page][x] |= (1 << bit);
    } else {
        framebuffer[page][x] &= ~(1 << bit);
    }
}

/* Ein Zeichen (5x7) im Framebuffer an Position (x, y) rendern */
static void display_draw_char(int x, int y, char c)
{
    if (c < 0x20 || c > 0x7F) c = '_';
    int idx = c - 0x20;

    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[idx][col];
        for (int row = 0; row < 8; row++) {
            if (line & (1 << row)) {
                display_set_pixel(x + col, y + row, true);
            }
        }
    }
}

/* String an Position (x, y) rendern (6 Pixel pro Zeichen inkl. Spacing) */
static void display_draw_string(int x, int y, const char *str)
{
    while (*str) {
        display_draw_char(x, y, *str);
        x += 6; /* 5 Pixel Font + 1 Pixel Abstand */
        if (x + 6 > DISPLAY_WIDTH) break;
        str++;
    }
}

/* Display Clear (Framebuffer loeschen + senden) */
void display_clear(void)
{
    if (!display_initialized) return;

    memset(framebuffer, 0, sizeof(framebuffer));
    display_update();
}

/* Splash-Screen */
void display_show_splash(void)
{
    if (!display_initialized) return;

    display_clear();
    display_draw_string(20, 0, "Heltec LoRa");
    display_draw_string(8, 16, "WiFi LoRa 32 V3");
    display_draw_string(12, 32, "ESP32-S3");
    display_draw_string(4, 48, "SX1262 868MHz");

    /* Zeichne eine horizontale Linie unter dem Titel */
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
        display_set_pixel(x, 12, true);
        display_set_pixel(x, 13, true);
    }

    display_update();
}

/* Status-Text anzeigen (oben, gross) */
void display_show_status(const char *status)
{
    if (!display_initialized) return;

    /* Erste 3 Zeilen loeschen */
    memset(framebuffer[0], 0, DISPLAY_WIDTH);
    memset(framebuffer[1], 0, DISPLAY_WIDTH);
    memset(framebuffer[2], 0, DISPLAY_WIDTH);

    display_draw_string(0, 0, status);
    display_update();
}

/* Fehler anzeigen (rot invertiert = leuchtend) */
void display_show_error(const char *error)
{
    if (!display_initialized) return;

    display_clear();
    display_draw_string(0, 0, "ERROR!");
    display_draw_string(0, 16, error);
    display_update();
}

/* Empfangene LoRa-Nachricht anzeigen */
void display_show_lora_rx(const lora_message_t *msg)
{
    if (!display_initialized || !msg) return;

    /* Display clear */
    display_clear();

    char line[20];
    snprintf(line, sizeof(line), "RX Type: 0x%02X", msg->type);
    display_draw_string(0, 0, line);

    snprintf(line, sizeof(line), "Node: %u", msg->node_id);
    display_draw_string(0, 10, line);

    snprintf(line, sizeof(line), "RSSI: %d dBm", msg->rssi);
    display_draw_string(0, 20, line);

    snprintf(line, sizeof(line), "SNR: %d dB", msg->snr);
    display_draw_string(0, 30, line);

    /* Erste 20 Bytes Payload als Hex anzeigen */
    char hex[50] = {0};
    int pos = 0;
    for (int i = 0; i < msg->payload_len && i < 10 && pos < 48; i++) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", msg->payload[i]);
    }
    display_draw_string(0, 40, hex);

    snprintf(line, sizeof(line), "Len: %u", msg->payload_len);
    display_draw_string(0, 54, line);

    display_update();
}

/* Gesendete LoRa-Nachricht anzeigen */
void display_show_lora_tx(const lora_message_t *msg)
{
    if (!display_initialized || !msg) return;

    display_clear();

    char line[20];
    snprintf(line, sizeof(line), "TX Type: 0x%02X", msg->type);
    display_draw_string(0, 0, line);

    snprintf(line, sizeof(line), "Node: %u", msg->node_id);
    display_draw_string(0, 10, line);

    snprintf(line, sizeof(line), "Len: %u bytes", msg->payload_len);
    display_draw_string(0, 20, line);

    display_draw_string(0, 40, "Sending...");
    display_update();
}

/* Text in Zeile X (0-7, 8px Zeilenhoehe) */
void display_show_line(int line, const char *text)
{
    if (!display_initialized) return;
    if (line < 0 || line >= 8) return;

    int y = line * 8;

    /* Nur die betroffene Page im Framebuffer loeschen */
    memset(framebuffer[line], 0, DISPLAY_WIDTH);

    display_draw_string(0, y, text);
    display_update_page(line);
}