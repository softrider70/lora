# LoRa — Heltec WiFi LoRa 32 V3 mit GPS

Zwei **Heltec WiFi LoRa 32 V3** (ESP32-S3 + SX1262) tauschen über LoRa Nachrichten
aus. Ein Board bekommt ein **GPS-Modul (GY-NE06MV2, u-blox NEO-6M)** und sendet
seine Position; beide Boards zeigen die Position auf dem OLED an.

- **Sensor-Node** (mit GPS): COM8
- **Anzeige-Node** (ohne GPS): COM3

Beide Boards laufen mit **derselben Firmware**. Ob ein Fix vorliegt, erkennt das
Board selbst — ohne GPS sendet es nur ein Lebenszeichen (Status).

## Hardware

| Komponente | Wert |
|---|---|
| Chip | ESP32-S3N8 (8 MB Flash, kein PSRAM) |
| LoRa | SX1262, 868 MHz, SF7, BW125, 14 dBm |
| Display | OLED SSD1306 128x64, I2C, Adresse 0x3C |
| Board-LED | GPIO35 |
| Taster | GPIO0 (BOOT) |
| Konsole | UART0 über CP2102 (GPIO43/44), 115200 Baud |

## GPS anschließen (GY-NE06MV2)

| GPS-Modul | Heltec V3 | Header |
|---|---|---|
| **TX** (Modul sendet) | **GPIO4** (ESP-Empfang) | J3 Pin 15 |
| **RX** (Modul empfängt) | **GPIO5** (ESP-Senden) | J3 Pin 16 |
| VCC | 5V (oder 3V3) | J2 Pin 2 (bzw. J3 Pin 2/3) |
| GND | GND | J3 Pin 1 |

Die Leitungen werden gekreuzt: **TX des Moduls auf GPIO4**, **RX des Moduls auf
GPIO5**. Für reines Auswerten reichen TX, VCC und GND; GPIO5 wird nur gebraucht,
wenn man das Modul konfigurieren will.

Das Modul sendet NMEA-0183 mit 9600 Baud. Ein NEO-6M mit Backup-Batterie kann
eine andere Baudrate behalten — der Treiber sucht deshalb der Reihe nach
9600, 38400, 57600, 115200 und 4800 Baud ab, bis Daten ankommen.

Freie Pins am Board: GPIO1–7, GPIO38, GPIO45–48. Belegt sind LoRa (8–14),
OLED intern (17/18, RST 21, Versorgung über Vext 36), LED 35, ADC_Ctrl 37,
Taster 0, Konsole 43/44, USB 19/20, interner Flash 26–32.

## Anzeige (8 Zeilen à 21 Zeichen)

```
N217 B10 9Sat      Node-ID, Build-Nummer, Satelliten
Lat +49.1234567    eigene Breite (7 Stellen ~ 1 cm)
Lon +8.1234567     eigene Länge
Alt 123m HDOP 0.9  Höhe und Güte
TX#12 -80dBm SN7dB letzter Sendevorgang
RX N55 -95dBm      Funkstatus der Gegenseite
Lat +48.9876543    empfangene Breite
Lon +9.1234567     empfangene Länge
```

Ohne Fix stehen in Zeile 1–3 „warte auf GPS-Fix", Satelliten/Baudrate und die
IP-Adresse.

## Nachrichten zwischen den Nodes

| Typ | Wert | Inhalt |
|---|---|---|
| `LORA_MSG_TYPE_STATUS` | 0x02 | Zähler (4 Byte) + Chip-Temperatur, wenn kein Fix |
| `LORA_MSG_TYPE_PING` | 0x03 | Ping, wird mit PONG beantwortet |
| `LORA_MSG_TYPE_PONG` | 0x04 | Antwort auf Ping |
| `LORA_MSG_TYPE_GPS` | 0x07 | Position, 12 Byte: lat/lon als int32 (Grad × 10^7), Höhe als int16 (Meter), Satelliten, HDOP × 10 |

Die Position wird im Takt von `LORA_SEND_INTERVAL_MS` (5 s) gesendet, sobald ein
Fix vorliegt.

## Bauen und flashen

```powershell
# Bauen (erhoeht die Build-Nummer, siehe unten)
. .\activate-esp-idf.ps1
idf.py build

# Sensor-Node (COM8, mit GPS)
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode usb -UsbPort COM8

# Beide Boards nacheinander (COM8 = Sensor, COM3 = Anzeige)
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode beide

# OTA (nach dem ersten USB-Flash)
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode ota

# Monitor des Sensor-Nodes
idf.py -p COM8 monitor
```

Das Flash-Skript prüft, ob der Port wirklich angeschlossen ist (PnP-Status OK),
beendet verwaiste `idf_monitor`-Prozesse und wertet den Exit-Code aus.

## Build-Nummer

`tools/increment_build.py` zählt bei jedem Build hoch und schreibt
`include/version.h` und `.build_number`. Das ist als CMake-Ziel `lora_version`
eingebunden, läuft also ohne Zutun. Die Nummer steht an drei Stellen:

1. Im Log als **letzte Startmeldung**: `Heltec WiFi LoRa 32 V3 bereit - Build 10`
2. Auf dem **Display** in Zeile 0 (`N217 B10 9Sat`)
3. In `include/version.h`

MAJOR/MINOR stehen in `include/config.h`, BUILD kommt aus dem Zähler.

## WiFi und OTA

- Ohne gespeicherte Zugangsdaten startet ein Access Point `LoRa-AP` mit Captive
  Portal auf `http://10.1.1.1` (Port 80).
- Der OTA-Webserver läuft auf **Port 8080**: `http://lora-node.local:8080`
  (mDNS) bzw. `http://10.1.1.1:8080`. Port 80 und der Steuerport 32768 sind vom
  Captive Portal belegt — deshalb 8080 und 32769.
- Langer Tastendruck auf BOOT (> 3 s) löscht die WiFi-Zugangsdaten.

## Projektstruktur

```
lora/
├── components/main/
│   ├── main.c            Programm: Tasks, Anzeige, GPS-Verpackung, RX-Auswertung
│   ├── gps.c/.h          GPS-Treiber (UART, NMEA-GGA, Baudraten-Suche)
│   ├── lora.c            SX1262-Treiber (SPI, DIO1-Interrupt)
│   ├── display.c         SSD1306 über I2C, Framebuffer, 5x7-Font, I2C-Scan
│   ├── wifi.c            WiFi-Manager und Captive Portal
│   ├── ota.c             OTA-Webserver (Port 8080)
│   ├── nvs_config.c      NVS-Konfiguration (Node-ID, Zugangsdaten)
│   ├── heap_monitor.c    Heap-Statistik alle 10 s
│   └── stack_monitor.c   Stack-Statistik alle 15 s
├── include/              Header und Hardware-Konfiguration (config.h)
├── tools/                Build-, Flash- und Zaehlskripte
├── partitions.csv        8 MB Flash mit ota_0/ota_1 und lora_data
└── sdkconfig.defaults    Board-Konfiguration
```

## Bekannte Befunde (gemessen, nicht vermutet)

Alles am 2026-09-25 aus den Logs der beiden Boards belegt (Build 8–17). Die MAC
`70:AF:09:xx:xx:xx` (Node-ID 217) gehört dem Board, das jetzt als **Anzeige-Node
auf COM3** hängt, die MAC `70:AF:09:xx:xx:xx` (Node-ID 9) dem **Sensor-Node auf
COM8**.

- **Button-Task hatte 1024 Byte Stack** → „A stack overflow in task button",
  das Board startete in einer Schleife neu. Ebenso `stk_mon` und `display`.
  Ursache war `configMINIMAL_STACK_SIZE + 256` in den Monitor-Tasks — das sind
  Bytes, nicht Wörter. Jetzt: 4096 / 3072 / 4096. Seitdem kein Neustart mehr.
- **OTA-Server auf Port 80** startete nicht („error in listen (112)"), weil das
  Captive Portal Port 80 und Steuerport 32768 belegt. Jetzt 8080 / 32769.
- **OLED antwortete nicht** (`ESP_ERR_INVALID_RESPONSE`, auf beiden Boards).
  Ursache war nicht die Adresse, sondern der **Reset**: der SSD1306 hängt an
  GPIO21 und blieb ohne Freigabe im Reset; zusätzlich fehlte die Versorgung über
  **Vext (GPIO36, LOW = ein)**. Seit beidem steht im Log
  `Display initialisiert (128x64, I2C Addr 0x3C)` — auf beiden Boards. Die
  ursprünglich konfigurierten GPIO41/42 waren falsch (JTAG-Pins MTDI/MTMS),
  richtig sind GPIO17 (SDA) / GPIO18 (SCL).

Die Funkübertragung läuft: beide Nodes senden alle 5 s
(`Kein GPS-Fix - sende Status #2`, SX1262 initialisiert mit 868 MHz/SF7).

Der GPS-Empfänger liefert NMEA (`NMEA-Empfang bei 9600 Baud`, 0 Prüfsummenfehler),
hatte drinnen aber noch keinen Fix: `Sat 0, Qual 0, HDOP 99.9`. Qual 0 heißt laut
NMEA ausdrücklich „kein Fix" — das Modul braucht freie Sicht zum Himmel. Der
GPS-Task meldet diesen Stand alle 10 s, damit man den Unterschied zwischen
„kein Empfang" und „Empfang, aber kein Fix" im Log sieht.