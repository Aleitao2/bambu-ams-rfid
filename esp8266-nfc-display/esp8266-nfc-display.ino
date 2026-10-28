// ================================================================
// main.ino – NFC tag reader + HX8357 display on ESP8266
//
// HARDWARE (fixed by the custom driver – not changeable):
//   HSPI MISO → GPIO12   HSPI MOSI → GPIO13
//   HSPI CLK  → GPIO14   HSPI CS   → GPIO15
//   PN532 SDA → GPIO4    PN532 SCL → GPIO5
//
// REQUIRED LIBRARIES (Arduino Library Manager):
//   • Adafruit GFX Library
//   • PN532 by Elechouse (or Adafruit_PN532)
//   • NfcAdapter
//
// LOCAL FILES IN SKETCH FOLDER:
//   HX8357.h / HX8357.cpp / Commands.h  (your custom driver)
//
// LOCAL LIBRARY (in Arduino/libraries/sdk_drivers/):
//   spi.c, spi_interface.c, sdk_spi.h + their headers
// ================================================================

#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <NfcAdapter.h>
#include <Adafruit_GFX.h>
#include "HX8357.h"
#include "Commands.h"

extern "C" {
  #include "sdk_spi.h"
  #include "spi_interface.h"
}

// ── Configuration ───────────────────────────────────────────────
#define DISPLAY_TIMEOUT_MS  5000   // display stays on for 5 s after a tag read
#define I2C_SDA             4
#define I2C_SCL             5

// ── Color palette (RGB565) ──────────────────────────────────────
#define COLOR_BLACK    0x0000
#define COLOR_WHITE    0xFFFF
#define COLOR_GREEN    0x07E0
#define COLOR_CYAN     0x07FF
#define COLOR_ACCENT   0x4FE9   // the teal from your PlatformIO example
#define COLOR_DIMGREY  0x7BEF

// ── Objects ─────────────────────────────────────────────────────
PN532_I2C pn532_i2c(Wire);
NfcAdapter nfc(pn532_i2c);
HX8357     tft;

// ── State ───────────────────────────────────────────────────────
bool     displayIsOn  = false;
uint32_t displayOnAt  = 0;

// ── Display helpers ─────────────────────────────────────────────
void displayOn() {
  tft.sendCommand(HX8357_DISPON);
  displayIsOn = true;
  displayOnAt = millis();
}

void displayOff() {
  tft.sendCommand(HX8357_DISPOFF);
  displayIsOn = false;
}

void showWaiting() {
  tft.fillScreen(COLOR_BLACK);
  tft.setTextColor(COLOR_DIMGREY);
  tft.setTextSize(2);
  tft.setCursor(10, 220);
  tft.print("Waiting for tag...");
}

void showTag(const String& uid) {
  tft.fillScreen(COLOR_BLACK);

  // Header bar
  tft.fillRect(0, 0, 480, 50, COLOR_ACCENT);
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 16);
  tft.print("NFC TAG DETECTED");

  // UID label
  tft.setTextColor(COLOR_CYAN);
  tft.setTextSize(2);
  tft.setCursor(10, 80);
  tft.print("UID:");

  // UID value – large
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(3);
  tft.setCursor(10, 110);
  tft.print(uid);

  // Divider
  tft.drawFastHLine(10, 160, 460, COLOR_ACCENT);

  // Countdown hint
  tft.setTextColor(COLOR_DIMGREY);
  tft.setTextSize(1);
  tft.setCursor(10, 175);
  tft.print("Display turns off in 5 seconds");
}

// ── Setup ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[BOOT] Starting...");

  // I2C for PN532
  Wire.begin(I2C_SDA, I2C_SCL);

  // Display init
  Serial.println("[DISPLAY] Initializing...");
  tft.begin();
  tft.setRotation(LANDSCAPE_RIGHT);
  showWaiting();
  displayOn();
  Serial.println("[DISPLAY] Ready.");

  // NFC init
  Serial.println("[NFC] Initializing...");
  nfc.begin();
  Serial.println("[NFC] Ready. Hold a tag near the reader.");
}

// ── Loop ─────────────────────────────────────────────────────────
void loop() {
  yield();  // prevent WDT reset (critical on ESP8266)

  // Auto-off timeout
  if (displayIsOn && (millis() - displayOnAt >= DISPLAY_TIMEOUT_MS)) {
    Serial.println("[DISPLAY] Timeout – turning off.");
    displayOff();
  }

  // NFC poll
  if (nfc.tagPresent()) {
    NfcTag tag = nfc.read();
    String uid = tag.getUidString();

    Serial.print("[NFC] Tag: ");
    Serial.println(uid);

    // Wake display if needed, show UID, reset timer
    if (!displayIsOn) displayOn();
    showTag(uid);
    displayOnAt = millis();  // restart the 5-second countdown
  }

  delay(100);  // avoid flooding the I2C bus
}