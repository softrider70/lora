# AGENTS.md — Projektwissen `lora`

Diese Datei gehört zum Projekt, nicht zum Benutzerprofil. Regeln für alle
Projekte stehen in `~/.copilot/instructions/projekt-basis.instructions.md`.

## Was das Projekt ist

Zwei **Heltec WiFi LoRa 32 V3** (ESP32-S3 + SX1262). Ein Board mit **GPS
(GY-NE06MV2)**, beide zeigen Positionen auf dem OLED an. Gleiche Firmware für
beide Boards; GPS-Vorhandensein wird zur Laufzeit erkannt.

- Sensor-Node mit GPS: **COM3**
- Anzeige-Node ohne GPS: **COM8**

## Bauen und flashen

```powershell
# Umgebung
. .\activate-esp-idf.ps1          # entdoppelt den PATH

# Bauen
idf.py build

# Beide Boards
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode beide

# Ein Board
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode usb -UsbPort COM3

# OTA (Port 8080, nicht 80)
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode ota
```

- ESP-IDF: `C:\Users\user\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf`
  (**v6.1-dev**), Target `esp32s3`. VS-Code-Setting `idf.currentSetup` zeigt
  dorthin; vorher stand dort `C:\esp\v6.0\esp-idf`, was nicht zum Build passte.
- Der Build-Zähler läuft über das CMake-Ziel `lora_version` in
  `components/main/CMakeLists.txt`. **Nicht** zusätzlich von Hand
  `tools/increment_build.py` aufrufen — das zählt doppelt.
- `include/version.h` und `.build_number` werden erzeugt; `version.h` steht in
  `.gitignore`, `.build_number` wird von `build-and-commit.ps1` bewusst
  eingecheckt (Abweichung von der Profilvorgabe, damit die Build-Nummer im
  Verlauf sichtbar bleibt).

## Log mitlesen (Beweis statt Vermutung)

Der CP2102 hängt wie üblich an DTR/RTS: **DTR tief ziehen ist keine gute Idee**,
das ist mit GPIO0 verbunden und der Button-Task deutet es als Tastendruck.

```python
import serial, time
s = serial.Serial("COM3", 115200, timeout=0.3)
s.dtr = False      # DTR aus lassen
s.rts = True       # Reset über RTS
time.sleep(0.15)
s.rts = False
s.reset_input_buffer()
out = b""
t0 = time.time()
while time.time() - t0 < 15:
    out += s.read(4096)
s.close()
open("build/boot.log", "wb").write(out)
```

Ausgabe in eine Datei schreiben: Die Boot-ROM-Bytes lassen sich auf der Konsole
nicht als cp1252 ausgeben (`UnicodeEncodeError`).

## Hardware-Belegung (verifiziert)

| Funktion | Pins |
|---|---|
| LoRa SX1262 | SCK9, MISO11, MOSI10, NSS8, RST12, BUSY13, DIO1 14 |
| GPS NEO-6M | TX→GPIO4 (ESP-RX), RX→GPIO5 (ESP-TX) |
| Konsole UART0 | GPIO43/44 über CP2102 |
| LED / Taster | GPIO35 / GPIO0 |
| OLED | SDA/SCL intern (17/18 vermutet), RST 21, Versorgung Vext 36 |
| frei | GPIO1–7, 38, 45–48 |
| belegt intern | Flash 26–32 |

GPIO3/45/46 sind Strapping-Pins und für externe Signale ungeeignet.

## Bekannte Fehler und ihre Ursachen (alle gemessen)

1. **Stack-Overflow in `button`, `stk_mon`, `display`** → Board startete alle
   paar Sekunden neu. Ursache: Task-Stacks von 1024/2048 Byte;
   `configMINIMAL_STACK_SIZE + 256` sind Bytes, nicht Wörter. Behoben durch
   `TASK_STACK_BUTTON`/`TASK_STACK_MONITOR`/`TASK_STACK_DISPLAY`.
2. **OTA-Server startete nicht** (`error in listen (112)`, später
   `error in creating ctrl socket (112)`). Captive Portal belegt Port 80 und
   Steuerport 32768 → OTA nutzt 8080 und 32769.
3. **OLED antwortet nicht** (`ESP_ERR_INVALID_RESPONSE`), auch nach Umstellung
   von GPIO41/42 auf GPIO17/18 und mit Vext ein. Der eingebaute I2C-Scan findet
   auf beiden Pin-Paaren keinen Chip. Offen — vermutlich Hardware am Board mit
   COM3. Nächster Schritt: Ruhepegel im Log ansehen (SDA/SCL = 1 heißt Bus frei).
4. `sdkconfig.defaults` enthält veraltete Symbole (`ESP_INT_WDT_INIT`,
   `ESP_WDT_INIT`, `MDNS_ENABLE_NETWORKING`) — Kconfig warnt, sonst harmlos.

## Arbeitsweise in diesem Projekt

- Erst Log lesen, dann ändern. Jede Behauptung über das Board muss aus
  `build/boot.log` belegbar sein.
- Nach jeder Änderung bauen, flashen und die **Build-Nummer im Log** prüfen —
  sonst ist nicht sicher, welcher Stand läuft.
- Kleinste passende Änderung; bestehende Module (WiFi, OTA, Monitore) bleiben.
