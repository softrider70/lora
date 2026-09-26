# AGENTS.md — Projektwissen `lora`

Diese Datei gehört zum Projekt, nicht zum Benutzerprofil. Regeln für alle
Projekte stehen in `~/.copilot/instructions/projekt-basis.instructions.md`.

## Was das Projekt ist

Zwei **Heltec WiFi LoRa 32 V3** (ESP32-S3 + SX1262). Ein Board mit **GPS
(GY-NE06MV2)**, beide zeigen Positionen auf dem OLED an. Gleiche Firmware für
beide Boards; GPS-Vorhandensein wird zur Laufzeit erkannt.

- Sensor-Node mit GPS: **COM8**
- Anzeige-Node ohne GPS: **COM3**

## Bauen und flashen

```powershell
# Umgebung
. .\activate-esp-idf.ps1          # entdoppelt den PATH

# Bauen
idf.py build

# Beide Boards
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode beide

# Ein Board (COM8 = Sensor mit GPS, COM3 = Anzeige)
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode usb -UsbPort COM8

# OTA (Port 8080, nicht 80)
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode ota
```

- ESP-IDF: `C:\Users\<Benutzer>\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf`
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
s = serial.Serial("COM8", 115200, timeout=0.3)   # Sensor-Node mit GPS
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
| LED (stillgelegt, aus) / Taster | GPIO35 / GPIO0 |
| OLED | SDA 17 / SCL 18 (intern), RST 21, Versorgung Vext 36 (LOW = ein) |
| frei | GPIO1–7, 38, 45–48 |
| belegt intern | Flash 26–32 |

GPIO3/45/46 sind Strapping-Pins und für externe Signale ungeeignet.

Die weisse Board-LED (GPIO35) ist **stillgelegt und bleibt aus** (Build 125):
Der Code schaltete sie bei jedem Empfang ein, der Blink-Timer lief aber nur
einmal beim Start - dadurch leuchtete sie dauerhaft. GPIO35 wird nur noch als
Ausgang auf LOW gehalten.

## Bekannte Fehler und ihre Ursachen (alle gemessen)

1. **Stack-Overflow in `button`, `stk_mon`, `display`** → Board startete alle
   paar Sekunden neu. Ursache: Task-Stacks von 1024/2048 Byte;
   `configMINIMAL_STACK_SIZE + 256` sind Bytes, nicht Wörter. Behoben durch
   `TASK_STACK_BUTTON`/`TASK_STACK_MONITOR`/`TASK_STACK_DISPLAY`.
2. **OTA-Server startete nicht** (`error in listen (112)`, später
   `error in creating ctrl socket (112)`). Captive Portal belegt Port 80 und
   Steuerport 32768 → OTA nutzt 8080 und 32769.
3. **OLED antwortete nicht** (`ESP_ERR_INVALID_RESPONSE`, auf beiden Boards).
   Ursache war der **Reset**, nicht die Adresse: der SSD1306 hängt an GPIO21 und
   blieb ohne Freigabe im Reset. Dazu fehlte die Versorgung über Vext (GPIO36,
   LOW = ein). Seit beidem: `Display initialisiert (128x64, I2C Addr 0x3C)`.
   Der erste Verdacht (GPIO41/42) war richtig: das sind JTAG-Pins (MTDI/MTMS),
   das OLED hängt an GPIO17 (SDA) / GPIO18 (SCL). Ist im Code so gesetzt.
4. `sdkconfig.defaults` enthält veraltete Symbole (`ESP_INT_WDT_INIT`,
   `ESP_WDT_INIT`, `MDNS_ENABLE_NETWORKING`) — Kconfig warnt, sonst harmlos.
5. **SX1262 sendet nicht — GELÖST (Build 122/123).** Der Funkverkehr läuft
   jetzt in **beide Richtungen**: COM8 (Node 9 mit GPS) sendet Positionen,
   COM3 (Node 217) empfängt sie (`RX: Type=0x07, Node=9` →
   `GPS von Node 9: +50.1234567 (3 Sat; Wert anonymisiert)`), und die Statusmeldungen von COM3
   kommen auf COM8 an. Die Ursachen (alle im Log belegt):
   1. **`ClearDeviceErrors` (0x07) braucht ZWEI Parameterbytes** (Inhalt egal).
      Vorher wurde nur das Kommando gesendet - das war wirkungslos, das
      `XOSC_START`-Flag klebte und der Chip lehnte `SetTx`/`SetRx` ab
      (Build 89: Clear alt → Fehler bleibt `0x0020`; Clear mit zwei Bytes →
      `0x0000`, danach `SetRx` → Status `0xD2` = Mode 5). Quelle: RadioLib
      (`clearDeviceErrors`, zwei NOP-Bytes) und LoRaMac-node.
   2. **Frequenzformel war falsch:** `f / 15625` (0x00D900) statt
      `f * 2^25 / 32 MHz` (= 0x36400000 für 868 MHz; RadioLib:
      `FREQUENCY_STEP_SIZE 0,9536743164`). Mit dem falschen Wert lockte die
      PLL nie (`PLL_LOCK`), sobald die Frequenz konfiguriert war (Build 101:
      `SetFs` vor der Konfiguration ok, danach `PLL_LOCK`). Mit 0x36400000:
      `SetFs` → Mode 4, `SetTx` → Mode 6, Fehler `0x0000` (Build 104).
   3. **Bandbreiten-Code:** 0x04 = 125 kHz, 0x05 = 250 kHz (SX126x-Tabelle
      laut RadioLib). Im Code stand 0x05 mit dem Kommentar "125 kHz".
   4. **`lora_dio1`-Task hatte 2048 Byte Stack** (Bytes, nicht Wörter!) →
      Überlauf zerstörte den Heap, Absturz (`LoadProhibited`) im nächsten
      `gpio_install_isr_service` (Build 116). Jetzt `TASK_STACK_LORA` (4096).
   5. **SPI nicht serialisiert:** main-Task und DIO1-Task sprachen parallel
      über denselben Bus (manuelles NSS!) → `assert spi_device_transmit`
      (Build 119). Jetzt serialisiert `spi_mutex` alle Chipzugriffe.
   6. **PA-Konfiguration war falsch - Ursache der schlechten Reichweite
      (gefunden 2026-09-26).** Im Code stand die SX1261-Konfiguration
      (`deviceSel 0x01, hpMax 0x00`), weil ein 16-Byte-Dump von Register
      0x0320 sich wie "SX1261 V2D 2D02" liest. Dieser Dump ist **keine**
      offizielle Chip-ID (dafür gibt es das Kommando `GetVersion` 0x42).
      Messung bei 30 cm Abstand, gleiche Boards:
      SX1261-Werte → **−109 dBm**, SX1262-Werte → **−26 dBm**.
      Das Modul ist also ein **SX1262**; mit den SX1261-Werten sendete es rund
      80 dB zu leise. Damit ist erklärbar, warum die Strecke zum Balkon
      zusammenbrach (dort kamen nur noch −106 bis −109 dBm an).
      **Belegt durch RadioLib:** in `SX1262.h` steht ausdrücklich, dass sich
      *alle* SX1262 als "SX1261" melden ("it seems that all SX1262 devices
      report as SX1261") - RadioLib erkennt den SX1262 deshalb genau an diesem
      String. Der Dump von 0x0320 taugt also **nicht** zur Unterscheidung.
      Endstand: `LORA_PA_SX1262 1`, `deviceSel 0x00`, `hpMax 0x07`,
      `LORA_TX_POWER 20` (Datenblatt V3: 21 ± 1 dBm).
   7. **OLED: Rauschbild und I2C-Aussetzer (gefunden 2026-09-26).**
      Zwei getrennte Ursachen:
      - **Rauschbild:** In `display_init()` stand `display_initialized = true`
        erst **nach** dem ersten Schreiben. `display_update()`, `display_clear()`
        und `display_update_page()` steigen bei `!display_initialized` sofort aus -
        der Bildspeicher wurde also nie gelöscht und das Panel zeigte den
        zufälligen Einschaltinhalt des SSD1306 (weißes Rauschen). Das Flag wird
        jetzt **vor** dem Schreiben gesetzt.
      - **Aussetzer:** `ESP_ERR_INVALID_RESPONSE` / `i2c.master: I2C software
        timeout`. Betroffen sind **nur die 129-Byte-Daten-transfers** (Kommandos
        mit 2 Byte laufen) und nur im Startfenster (783-843 ms), also während der
        LoRa-Initialisierung mit ihren Stromspitzen. Danach läuft der Verkehr
        fehlerfrei: 2661 Transfers, 19 Fehlschläge - alle im Startfenster.
      - Kein Busdefekt: bei den Fehlern standen SDA/SCL auf `1 1` (Leitung frei),
        der SSD1306 gab nur kein ACK.
      - `i2c_master_bus_reset` im Fehlerpfad ist **falsch**: daraus wurde eine
        Lawine (74 Fehler/min statt ~20). Takt 100 kHz ist **schlechter** als
        400 kHz: damit scheitert schon das erste Kommando und der Init bricht ab.
      - Lösung: `display_send_page()` setzt vor **jedem** Versuch die Adresse neu
        und wiederholt bis zu 3×. Das Zurücksetzen ist nötig, weil der SSD1306 im
        Horizontal-Mode sonst hinter der letzten Spalte weiterzählt und die
        folgenden Seiten verschiebt. Kommandos werden ebenfalls bis 3× gesendet.
      - Offen (Hardware): Die Aussetzer in den Stromspitzen sind **abgefangen,
        nicht beseitigt**. Nächster Schritt: Vext (GPIO36) / 3,3 V beim Senden mit
        dem Oszilloskop messen, ggf. Stützkondensator am OLED.
      - Zähler: `Transfers ok / wiederholt / fehlgeschlagen` einmal je Minute im
        Log.
   Frühere Verdachtsfälle (SPI-Takt, Regler-Modus, DIO3-Register,
   Messtest-Varianten) sind damit erledigt. Die 380-mV-Messung am
   Metallbecher hat in die Irre geführt - der Oszillator läuft nachweislich
   (RX und TX auf 868 MHz). Der Messmodus `LORA_TCXO_MESSTEST` in `lora.c`
   ist aus (0); als letzte Stufe enthält er die "Abschlussprobe" (FS/TX mit
   korrigierter Frequenz und Chipmodus-Ausgabe) für spätere Prüfungen.
   (Details der Fehlersuche von Build 44 bis 87 stehen im Git-Verlauf dieser
   Datei; die dort genannten Ausschluesse sind mit den Erkenntnissen von
   Build 89-123 ueberholt.)
   **Verlauf der Fehlersuche (ueberholt - die Ursachen stehen oben):**
   weitere Messungen (Build 60–67):
   - Das Versionsregister meldet `SX1261 V2D 2D02` — auf dem Modul sitzt die
     leistungsschwache Variante (max. 15 dBm). Deren PA-Konfiguration
     (`deviceSel 0x01`, `hpMax 0x00`) ist jetzt gesetzt.
   - Der Selbsttest in `lora_init` (Aussendung **vor** WiFi und Tasks, Build 67)
     schlägt ebenfalls fehl (`TX-Timeout`, `XOSC_START`) → kein Versorgungs- oder
     Störproblem der laufenden Anlage.
   - Nach dem Start der Tasks antwortet der Chip nur noch mit Statusbytes
     (Version liest `0xB2`), vorher exakt (`SX12`).
   - Nächste Versuche: `SetRegulatorMode` auf DC-DC, XTA/XTB-Trim für
     TCXO-Betrieb, Reset + vollständige Neu-Konfiguration vor dem ersten Senden.
   - **Ebenfalls ohne Wirkung (Build 71):** `SetRegulatorMode` DC-DC und
     Reset + komplette Neu-Konfiguration unmittelbar vor dem Senden.
     `XOSC_START | PLL_LOCK` bleibt in jeder Phase stehen (Init, nach der
     Konfiguration, vor dem Empfang, beim Senden).
     **Damit ausgeschlossen:** TCXO-Spannungsstufe (9 Stufen), SPI-Takt (2/8 MHz),
     Versorgungslage (ruhiges System vs. laufende Anlage), Regler-Modus,
     Reihenfolge/Reset, Kalibrier-Kommandos, PA-Konfiguration.
     Es bleibt der **XTA/XTB-Trim** (Werte nicht geraten) oder die
     Oszillator-Beschaltung des Moduls selbst.
   - **Oszilloskop-Messung (2026-09-25):** Ein Pin des 4-poligen Metallbechers
     neben dem SX1262 folgt dem Testmuster, erreicht aber nur rund **380 mV**
     statt der konfigurierten 1,8 V - auf **beiden** Boards gleich, und die
     Spannungsstufe (1,8 V oder 3,3 V) ändert nichts. Der Pegel wird also
     festgehalten. Der Oszillator bekommt damit keine Versorgung und schwingt
     nicht an. **(Mit Build 122/123 widerlegt: der Oszillator läuft - RX und
     TX arbeiten auf 868 MHz. Die 380-mV-Messung war ein Fehlschluss.)**
   - **DIO3-Register geprueft (Build 86/87, Verdacht widerlegt):** Adressen
     aus RadioLibs `SX126x_registers.h` (nicht geraten): `DIOX_OUT_ENABLE`
     0x0580 (**invertiert**: Bit 3 = 1 schaltet den Ausgang ab), `DIOX_IN_ENABLE`
     0x0583 (Bit 3 = 1 schaltet den Eingang ein), dazu 0x0582 (Drive-Stärke),
     0x0584/0x0585 (Pull-Up/Down). Befund aus dem Log, auf beiden Boards
     gleich: Nach dem Reset stehen **alle** DIOx-Register auf `0x00`, also im
     Zustand, den RadioLib im Paketmodus herstellt. Schreibzugriffe kommen an
     (`0x0580` liest nach dem Test `0x08` zurueck), und das Fehlerregister
     bleibt in **allen vier** Varianten `0x0020` (XOSC_START). Auch mit Bit 3
     in beiden moeglichen Zustaenden (0 und 1) aendert sich nichts - egal wie
     die Invertierung auszulegen ist. Weder Digitalausgang noch Pull-Widerstände
     halten den Oszillator ab; die Register sind **nicht** die Ursache der
     380 mV.
   - **Messtest im Code:** `LORA_TCXO_MESSTEST` in `lora.c`. Stand Build 86
     (COM8) / 87 (COM3): Registertest mit vier Varianten im Wechsel
     (0 unveraendert, A RadioLib-Paketmodus, B Ausgang aus, C Pulls aus).
     Je Runde: 150 ms aus (Reset), Register lesen/setzen, TCXO- und
     Kalibrier-Kommando, Fehlerwort ins Log, dann rund 480 ms TCXO an.
     Die restliche Anwendung startet in diesem Modus nicht. Zum Abschalten
     den Wert auf 0 setzen und beide neu flashen.
   - **Erledigt (Build 122/123):** Der Funkbetrieb läuft in beide Richtungen,
     siehe Punkt 5 oben. Als Diagnosewerkzeug bleibt der Messmodus
     `LORA_TCXO_MESSTEST` in `lora.c`: auf > 0 gesetzt (und neu geflasht)
     durchläuft er in `lora_init` die FS/TX-Probe mit Reset-Pulsmuster; auf 0
     startet die normale Anwendung.
   - **GPS ist dagegen bewiesen** (Build 60, Board COM8): `Sat 8, Qual 1,
     HDOP 1.6` und `Sende Position #0 (8 Sat)` — NMEA, Parser, Fix und
     Nutzlast-Aufbau funktionieren.

## Arbeitsweise in diesem Projekt

- Erst Log lesen, dann ändern. Jede Behauptung über das Board muss aus
  `build/boot.log` belegbar sein.
- Nach jeder Änderung bauen, flashen und die **Build-Nummer im Log** prüfen —
  sonst ist nicht sicher, welcher Stand läuft.
- Kleinste passende Änderung; bestehende Module (WiFi, OTA, Monitore) bleiben.
