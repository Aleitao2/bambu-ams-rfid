# Bambu AMS RFID Filament Tracker

![status](https://img.shields.io/badge/status-working-brightgreen) ![ESP32](https://img.shields.io/badge/ESP32-Arduino-blue) ![ESP8266](https://img.shields.io/badge/ESP8266-Arduino-blue) ![Home Assistant](https://img.shields.io/badge/Home%20Assistant-automations-41BDF5)

Automatic filament tracking for a **Bambu Lab P1S + AMS** using third-party (non-Bambu) spools.
Each spool carries a cheap NFC/RFID sticker. When a spool is inserted into an AMS tray, Home Assistant
asks an ESP32 to read the tag, looks the UID up in **Spoolman**, and assigns the right spool to the right
tray in **Bambuddy** — no manual selection in the slicer.

```
 AMS tray inserted ──► Home Assistant ──GET /scan?reader=A|B──► ESP32 (MFRC522 + PN532)
                              ▲                                        │
                              └────── POST webhook {tag_uid, reader} ◄─┘
                              │
                              ├──► Spoolman  (link spool ↔ tray)
                              └──► Bambuddy  (assign spool to AMS slot)
```

## What's in this repo

| Folder | What it is |
|---|---|
| [`esp32-ams-reader/`](esp32-ams-reader) | Main firmware. ESP32 with **two readers on one SPI bus** (MFRC522 for trays 1–2, PN532 for trays 3–4). Exposes an HTTP API for Home Assistant, posts tag UIDs to an HA webhook, has a **built-in web serial monitor** and **OTA updates**. |
| [`esp8266-nfc-display/`](esp8266-nfc-display) | Stand-alone tag reader: ESP8266 + PN532 (I²C) showing the scanned UID on a 3.5" **HX8357** display, with screen auto-off. Used to read/register new tags at the bench. |
| [`esphome/`](esphome) | Alternative ESPHome version of a single-reader tag scanner that fires a Home Assistant event. |
| [`home-assistant/`](home-assistant) | The automations, script and helpers that run the whole flow. |

## ESP32 AMS reader — features
- Two different RFID chips (MFRC522 + PN532) sharing one SPI bus with independent chip-selects
- HTTP endpoints: `/scan?reader=A|B&ignore=<uid>`, `/status`, `/` (web log UI), `/log`, `/log/clear`
- "Ignore UID" so the reader skips the neighbouring spool that is also in range
- Sends `TIMEOUT` when no tag is found so HA can rotate the spool and retry
- 200-line ring-buffer log mirrored from `Serial` to a web page — debug without a USB cable
- ArduinoOTA for wireless firmware updates

**Wiring (shared SPI):** SCK 18 · MISO 19 · MOSI 23 · MFRC522 CS 5 / RST 22 · PN532 CS 15 (PN532 DIP: SPI mode)

**Libraries:** `MFRC522` (GithubCommunity), `Adafruit PN532`, ESP32 Arduino core (WiFi, HTTPClient, WebServer, ArduinoOTA)

**Setup**
1. Copy `esp32-ams-reader/secrets.example.h` → `secrets.h` and fill in WiFi + a Home Assistant long-lived token.
2. Flash with Arduino IDE (board: ESP32 Dev Module).

## Home Assistant flow
| Automation | Role |
|---|---|
| `01_tray_insert_trigger_scan` | Tray goes Empty → filament: queue a scan, store the active tray, call the ESP32 |
| `02_scan_assign_filament_to_tray` | Webhook with UID: normalise UID, look up Spoolman, unlink old spool, link new one, assign in Bambuddy |
| `03_scan_timeout_spool_rotation` | Webhook with `TIMEOUT`: rotate the spool (`bambu_lab.read_rfid`) and retry up to N times |
| `04_tray_remove_unlink_spool` | Tray emptied: unlink the spool from Spoolman and Bambuddy |
| `05_spoolman_register_from_sidebar` | Opens the tag-registration dialog |
| `06_bambuddy_sync_ams_slots` | Mirrors Bambuddy slot presets / spool IDs into helpers for the dashboard |

`scripts/register_rfid_tag_to_spoolman.yaml` binds a new tag UID to a Spoolman spool ID.
`helpers.yaml` lists the `input_text` / `input_number` / `counter` helpers used.

> Not included: the `rest_command` definitions (Spoolman / Bambuddy / ESP32 endpoints) and the
> pyscript that writes `filament_tag_map.json`, since they contain local addresses.
> Entity IDs use `<printer_serial>` as a placeholder.

## ESP8266 NFC display
- ESP8266 + PN532 over I²C (SDA 4 / SCL 5) and an HX8357 display on HSPI (GPIO 12–15)
- Shows the UID of the last tag, blanks the screen after a timeout, wakes on the next tag
- The HX8357 9-bit SPI driver is adapted from
  [robin7331/malyan-m200-display-firmware](https://github.com/robin7331/malyan-m200-display-firmware) (MIT, see `LICENSE-HX8357-driver`);
  `sdk_drivers/` are Espressif ESP8266 NONOS SDK SPI drivers.

## Development history
This went through ~15 iterations: single MFRC522 → dual MFRC522 → MFRC522 + PN532 (I²C, then SPI) →
HTTP scan windows → web log → OTA + ignore-UID logic. Only the final versions are published here.
