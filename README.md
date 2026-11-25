<!-- G4MEOVER Heltec V4 Hub -->
<h1 align="center">G4MEOVER · Heltec V4 Hub</h1>
<p align="center"><a href="https://github.com/G4MEOVER18/heltec-v4-hub/actions/workflows/platformio-build.yml"><img src="https://github.com/G4MEOVER18/heltec-v4-hub/actions/workflows/platformio-build.yml/badge.svg" alt="CI"></a></p>

<p align="center"><b>Der Funk-Executor des G4MEOVER-Ökosystems</b><br>
ESP32-S3 + SX1262 LoRa + OLED + GNSS — vom Flipper Zero gesteuert.</p>

<p align="center"><b>// PWNED BY YOU</b></p>

---

> Nur für **autorisierte** Security-Forschung an eigener Hardware / in eigenen Netzen / CTF.
> Die aktiven Module (Deauth, Beacon-Spam, Evil-Portal, BLE-Spam) laufen mit Sicherheits-
> Timeouts und ausschließlich auf ausdrücklichen Befehl.

## Was ist das

Ein **Flipper-Hub** auf Basis der Heltec WiFi LoRa 32 **V4**. Der Flipper Zero steuert
das Board über GPIO-UART und löst damit Funk-Aktionen aus, die der Flipper selbst nicht
kann (2,4-GHz-WiFi/BLE, LoRa-Weitbereich, Drohnen-Remote-ID). Ergebnisse gehen über den
Rückkanal an den Flipper; zusätzlich funkt der Hub per **ESP-NOW** an die Satelliten-Nodes
(LilyGo/WROOM) und spricht **LoRaWAN/TTN** für Weitbereichs-Kommandos.

## Hardware & Anschluss

| Element | Detail |
|---|---|
| MCU | ESP32-S3R2, 16 MB Flash, native USB-CDC (Hardware, nicht TinyUSB) |
| Funk | SX1262 (LoRa/FSK), 2,4-GHz-WiFi, BLE (NimBLE) |
| Anzeige | OLED SSD1306/SD1315, Mehrseiten-Menü |
| GNSS | GPS über GNSS-Stecker (Serial2) |
| Flipper-Link | GPIO-UART: Flipper 13/14 ↔ ESP **GPIO47 (RX) / GPIO48 (TX)**, 115200 Baud |
| Steuerkanäle | UART (Flipper), BLE-GATT (`ble_ctl`), USB-Serial — alle sprechen `ukfe_rf` |

## Fähigkeiten (aktueller Firmware-Stand)

**WiFi** (`wifi_recon` / `wifi_attack`)
- Scan, Wardrive (BSSID/SSID/GPS), Probe-Sniff, PacketMon, Pwnagotchi-Modus
- Handshake-Capture (EAPOL, Reauth per gezieltem Deauth erzwungen)
- Deauth, Beacon-Spam, **Evil-Portal** (Evil Twin mit variablem HTML-Skelett)

**BLE** (`ble_ctl` / `ble_spam`)
- Scan (benannte Geräte, RSSI), BLE-Spam (rotierende Zufallsadresse pro Advertisement)

**Drohnen-Remote-ID** (`RidScan`)
- ODID-Dekoder über **BLE (sofort)** + **WiFi-Beacon (nebenläufig)**; Location-Nachrichten
  werden mit gecachter UAS-ID gelabelt

**LoRa / LoRaWAN** (`lorawan_link`)
- Freies LoRa-Terminal (SX1262), FSK-Restore für den `ukfe_rf`-Empfang
- LoRaWAN/TTN: Downlink-Kommandokanal + Uplink-Ergebnismeldung, Nonces im eigenen NVS-Namespace

**Infrastruktur**
- `ukfe_rf`-Frames ([LEN][0x47][0x01]…) verifiziert, Antworten über alle drei Kanäle
- ESP-NOW-Relay an die Satelliten (`ESPNOW_CHANNEL=1`), ACK-Rückweg zum Flipper
- OLED-Status (Aktion · letzter Befehl · Zähler · Toast)

## Bauen & Flashen

PlatformIO (ESP32-Arduino, `espressif32@6.5.0`):

```
pio run                 # bauen
pio run -t upload       # flashen (Heltec V4 per USB verbunden)
pio device monitor      # Serial 115200
```

> Board-Target ist `esp32-s3-devkitc-1` (generisch), da die V4-Boarddef in PIO 6.5.0 fehlt —
> die Pins sind V3-kompatibel gesetzt (siehe `platformio.ini`).

## Experimentier-Roadmap (geplant)

Ideen, die das Spektrum weiter ausreizen — Umsetzung, sobald das Board wieder angeschlossen ist:

- **LoRa-Spektrum-Lite:** RSSI-Sweep über das 868/915-Band als Balkenansicht im Flipper-UI
- **GNSS-Telemetrie-Panel:** Live-Position/Fix/Satellitenzahl als eigene Flipper-Seite
- **WiFi/BLE-Live-Feed:** Recon-Ergebnisse streamen statt in Log — direkt ins Flipper-Panel
- **LoRa-Mesh-Karte:** empfangene Knoten/RSSI als kleine Kartenansicht
- **RID-Aggregator:** erkannte Drohnen mit Pfad/letzter Position sammeln

## Rolle im Ökosystem

```
Flipper Zero  ──UART/BLE──►  Heltec V4 Hub  ──ESP-NOW──►  Satelliten (LilyGo/WROOM)
                                   │
                                   └── LoRaWAN/TTN  (Weitbereich, Downlink-Kommandos)
```

## Support

> **PayPal**: https://paypal.me/Freakbank1
> **Bitcoin**: `39vZWmnUwDReQ15BwqQXzyqVQ6U8LardEf`

## Lizenz

Eigenes Projekt des Betreibers. Nutzung der eingebundenen Bibliotheken (U8g2, TinyGPSPlus,
RadioLib, NimBLE-Arduino) unter deren jeweiligen Lizenzen.
