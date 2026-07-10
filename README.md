# LoRa — ESP32-S3 LoRa Kommunikation (Heltec WiFi LoRa 32 V3)

Zwei **Heltec WiFi LoRa 32 V3** Module tauschen ueber **LoRa-Funk (SX1262)** Daten aus. Beide Module zeigen Status auf dem integrierten **OLED Display (SSD1306)** und unterstuetzen **OTA-Firmware-Updates**.

## Board: Heltec WiFi LoRa 32 V3

| Komponente | Spezifikation |
|---|---|
| **Chip** | ESP32-S3 (Dual-Core Xtensa LX7, bis 240MHz) |
| **LoRa** | SX1262 (868/915MHz, SPI) |
| **Display** | OLED SSD1306 128x64 (I2C: SDA=GPIO41, SCL=GPIO42) |
| **LED** | GPIO35 |
| **Taster** | GPIO0 (BOOT) |
| **Flash** | 16MB Quad-Flash (QIO, 80MHz) |
| **PSRAM** | 8MB Octal (OPI, 80MHz) |
| **USB** | USB-C (USB Serial/JTAG native) |
| **WiFi** | 802.11 b/g/n, AP + STA Modus |

## Features

- **LoRa-Kommunikation** zwischen zwei Nodes (SX1262, 868MHz EU-Band)
- **OLED Display** mit Echtzeit-Status (TX/RX, RSSI, SNR)
- **OTA-Updates** ueber eingebetteten Webserver (minimale Web-UI)
- **WiFi Captive Portal** (AP bei fehlenden Credentials)
- **NVS-Konfigurationsspeicher** (Node-ID, WiFi-Credentials)
- **Stack- und Heap-Monitoring**
- **ESP-IDF 6.1, FreeRTOS (Dual-Core)**

## Kommunikation

Nachrichtentypen zwischen den Nodes:

| Typ | Wert | Beschreibung |
|---|---|---|
| `LORA_MSG_TYPE_SENSOR` | 0x01 | Sensorwerte (Temperatur, Spannung, Heap, Uptime) |
| `LORA_MSG_TYPE_STATUS` | 0x02 | Statusmeldung |
| `LORA_MSG_TYPE_PING` | 0x03 | Ping |
| `LORA_MSG_TYPE_PONG` | 0x04 | Pong |
| `LORA_MSG_TYPE_ALARM` | 0x06 | Alarm |

## Schnellstart

### Build
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-and-commit.ps1
```

### Flash (initial)
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode usb -UsbPort COM11
```

### OTA Flash (nach erstem Flash)
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode ota
```

### Monitor
```bash
idf.py -p COM11 monitor
```

## OTA-WebUI

Nach erfolgreichem WiFi-Verbindungsaufbau ist das Board erreichbar:
- **mDNS:** `http://lora-node.local`
- **AP-Modus:** `http://10.1.1.1`
- OTA-Update per `Upload & Flashen` (nur `.bin`-Dateien)

## Projektstruktur

```
lora/
├── components/main/       Quellcode
│   ├── main.c             Hauptprogramm
│   ├── lora.c             SX1262 LoRa-Treiber
│   ├── display.c          SSD1306 OLED via I2C
│   ├── wifi.c             WiFi-Manager
│   ├── ota.c              OTA-Webserver
│   ├── nvs_config.c       NVS-Konfigurationsspeicher
│   ├── stack_monitor.c    Stack-Ueberwachung
│   └── heap_monitor.c     Heap-Ueberwachung
├── include/               Header
│   ├── config.h           Hardware-Konfiguration
│   ├── lora.h             LoRa-API
│   ├── display.h          Display-API
│   ├── wifi.h             WiFi-API
│   ├── ota.h              OTA-API
│   ├── nvs_config.h       NVS-API
│   └── version.h.in       Version-Template
├── tools/                 Build-Skripte
│   ├── build-and-commit.ps1
│   ├── flash-mode.ps1
│   └── increment_build.py
├── CMakeLists.txt         ESP-IDF Projekt
├── sdkconfig.defaults     Board-Konfiguration
├── partitions.csv         OTA-Partitionen (16MB)
└── activate-esp-idf.ps1   ESP-IDF aktivieren
```

## Konfiguration

Alle wichtigen Parameter in `include/config.h`:
- **LoRa-Frequenz, SF, BW, Sendeleistung**
- **Display-Pins (I2C) und Timing**
- **WiFi-AP-SSID, Timeouts**
- **OTA-Port, mDNS-Hostname**
- **Task-Stacks und Prioritaeten**

## Versionierung

`MAJOR.MINOR.BUILD` — automatisch via `tools/increment_build.py`:
- MAJOR/MINOR in `config.h` (`APP_VERSION_MAJOR`, `APP_VERSION_MINOR`)
- BUILD wird bei jedem Build inkrementiert
- Bei Aenderung von MAJOR/MINOR wird BUILD auf 0 zurueckgesetzt