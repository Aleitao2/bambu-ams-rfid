/*
  AMS Dual RFID Reader → Home Assistant
  ──────────────────────────────────────
  Hardware : ESP32
             Reader A (Trays 1 & 2) → MFRC522   via SPI  CS=GPIO5
             Reader B (Trays 3 & 4) → PN532      via SPI  CS=GPIO15
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h> // NEW: Added for OTA Updates

// ── Pin config ────────────────────────────────────────────────
#define MFRC522_CS   5
#define MFRC522_RST  22
#define PN532_CS     15

// ── WiFi + Home Assistant credentials ─────────────────────────
// Copy secrets.example.h to secrets.h and fill in your own values.
// secrets.h is git-ignored so credentials never reach the repo.
#include "secrets.h"

const char* WEBHOOK_ID = "arduino_rfid_scanned";

// ── Scan window ───────────────────────────────────────────────
const unsigned long SCAN_WINDOW_MS = 5000;
const uint16_t PN532_POLL_MS = 150;

// ── Reader instances ──────────────────────────────────────────
MFRC522        mfrc522(MFRC522_CS, MFRC522_RST);
Adafruit_PN532 pn532(PN532_CS); 

// ── Scan state ────────────────────────────────────────────────
volatile int  activeReader = 0;
unsigned long scanStart    = 0;
String        ignoreUid    = ""; // NEW: Stores the UID to ignore during this scan

WebServer server(80);

// ═════════════════════════════════════════════════════════════
// WEB SERIAL MONITOR — ring buffer & helpers 
// ═════════════════════════════════════════════════════════════
#define LOG_BUFFER_LINES  200 
#define LOG_POLL_MS       1500 

static String logBuffer[LOG_BUFFER_LINES];
static int    logHead  = 0; 
static int    logCount = 0; 

static void logStore(const String &line) {
  logBuffer[logHead] = line;
  logHead = (logHead + 1) % LOG_BUFFER_LINES;
  if (logCount < LOG_BUFFER_LINES) logCount++;
}

void webLog(const String &msg)   { Serial.print(msg);   logStore(msg); }
void webLogln(const String &msg) { Serial.println(msg); logStore(msg); }
void webLogln()                  { Serial.println();    logStore("");   }

void webLog(const char   *msg) { webLog(String(msg));   }
void webLogln(const char *msg) { webLogln(String(msg)); }
void webLog(int    v, int base = DEC) { webLog(String(v, base));    }
void webLogln(int  v, int base = DEC) { webLogln(String(v, base));  }
void webLog(long   v, int base = DEC) { webLog(String(v, base));    }
void webLogln(long v, int base = DEC) { webLogln(String(v, base));  }
void webLog(unsigned long v, int base = DEC) { webLog(String(v, base));   }
void webLogln(unsigned long v, int base = DEC){ webLogln(String(v, base));}
void webLog(double v, int dec = 2) { webLog(String(v, dec));   }
void webLogln(double v, int dec = 2){ webLogln(String(v, dec)); }

// ─────────────────────────────────────────────────────────────
// UID helpers
// ─────────────────────────────────────────────────────────────
String mfrcUidStr(MFRC522::Uid &uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    if (i > 0) s += "-";
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

String pn532UidStr(uint8_t *uid, uint8_t len) {
  String s = "";
  for (uint8_t i = 0; i < len; i++) {
    if (i > 0) s += "-";
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// ─────────────────────────────────────────────────────────────
// POST tag UID to HA webhook
// ─────────────────────────────────────────────────────────────
void postTagToHA(const String &uid, int readerId) {
  if (WiFi.status() != WL_CONNECTED) {
    webLogln("[HTTP] WiFi not connected — skipping POST");
    return;
  }

  String url  = String(HA_HOST) + "/api/webhook/" + WEBHOOK_ID;
  String body = "{\"tag_uid\":\"" + uid + "\",\"reader_id\":" + String(readerId) + "}";

  webLogln("[HTTP] POST → " + url);
  webLogln("[HTTP] Body: " + body);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);

  int code = http.POST(body);
  webLogln("[HTTP] Response: " + String(code));
  http.end();
}

// ─────────────────────────────────────────────────────────────
// HTTP handlers
// ─────────────────────────────────────────────────────────────
void handleScan() {
  String readerParam = server.arg("reader");
  readerParam.toUpperCase();
  
  // Capture the UID we need to ignore (if HA sends it)
  ignoreUid = server.arg("ignore");
  ignoreUid.toUpperCase();

  if (readerParam == "A") {
    activeReader = 1;
    mfrc522.PCD_Init();           
    scanStart = millis();
    webLogln("[SCAN] Window A opened (MFRC522 — trays 1&2)");
    if (ignoreUid != "") webLogln("[SCAN] Ignoring UID: " + ignoreUid);
    server.send(200, "application/json", "{\"status\":\"scanning\",\"reader\":\"A\"}");

  } else if (readerParam == "B") {
    activeReader = 2;
    scanStart = millis();
    webLogln("[SCAN] Window B opened (PN532 — trays 3&4)");
    if (ignoreUid != "") webLogln("[SCAN] Ignoring UID: " + ignoreUid);
    server.send(200, "application/json", "{\"status\":\"scanning\",\"reader\":\"B\"}");

  } else {
    server.send(400, "application/json", "{\"error\":\"use ?reader=A or ?reader=B\"}");
  }
}

void handleStatus() {
  const char* rdr     = activeReader == 1 ? "A"
                      : activeReader == 2 ? "B"
                      : "none";
  unsigned long elapsed = activeReader ? (millis() - scanStart) : 0;
  String json = "{\"scanning\":"  + String(activeReader ? "true" : "false") +
                ",\"reader\":\""  + rdr + "\"" +
                ",\"elapsed_ms\":" + String(elapsed) + "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AMS RFID — Serial Monitor</title>
<style>
  :root {
    --bg:      #0d1117;
    --surface: #161b22;
    --border:  #30363d;
    --green:   #3fb950;
    --cyan:    #79c0ff;
    --yellow:  #e3b341;
    --red:     #f85149;
    --text:    #c9d1d9;
    --dim:     #6e7681;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'Courier New', Courier, monospace;
    font-size: 13px;
    height: 100vh;
    display: flex;
    flex-direction: column;
  }
  header {
    background: var(--surface);
    border-bottom: 1px solid var(--border);
    padding: 10px 16px;
    display: flex;
    align-items: center;
    gap: 12px;
    flex-shrink: 0;
  }
  header h1 {
    font-size: 13px;
    font-weight: 600;
    color: var(--cyan);
    letter-spacing: .08em;
    text-transform: uppercase;
    flex: 1;
  }
  #indicator {
    width: 8px; height: 8px;
    border-radius: 50%;
    background: var(--dim);
    transition: background .3s;
    flex-shrink: 0;
  }
  #indicator.live { background: var(--green); box-shadow: 0 0 6px var(--green); }
  #indicator.err  { background: var(--red);   box-shadow: 0 0 6px var(--red);   }
  .btn {
    background: transparent;
    border: 1px solid var(--border);
    color: var(--text);
    padding: 3px 10px;
    border-radius: 4px;
    cursor: pointer;
    font-family: inherit;
    font-size: 11px;
    letter-spacing: .06em;
    text-transform: uppercase;
    transition: border-color .2s, color .2s;
  }
  .btn:hover { border-color: var(--cyan); color: var(--cyan); }
  #autoscroll-label {
    display: flex; align-items: center; gap: 5px;
    font-size: 11px; color: var(--dim); user-select: none; cursor: pointer;
  }
  #log {
    flex: 1;
    overflow-y: auto;
    padding: 10px 16px;
    display: flex;
    flex-direction: column;
    gap: 1px;
  }
  .line {
    white-space: pre-wrap;
    word-break: break-all;
    line-height: 1.55;
    padding: 0 4px;
    border-radius: 2px;
  }
  .line.rfid-a  { color: var(--green);  }
  .line.rfid-b  { color: var(--cyan);   }
  .line.http    { color: var(--yellow); }
  .line.scan    { color: #d2a8ff;       }
  .line.error   { color: var(--red);    }
  .line.wifi    { color: #ffa657;       }
  .line.ready   { color: var(--green); font-weight: bold; }
  .line.dim     { color: var(--dim);    }
  footer {
    background: var(--surface);
    border-top: 1px solid var(--border);
    padding: 5px 16px;
    font-size: 11px;
    color: var(--dim);
    display: flex;
    gap: 16px;
    flex-shrink: 0;
  }
</style>
</head>
<body>
<header>
  <span id="indicator"></span>
  <h1>&#9654; AMS Dual RFID — Serial Monitor</h1>
  <label id="autoscroll-label">
    <input type="checkbox" id="autoscroll" checked> Auto-scroll
  </label>
  <button class="btn" onclick="clearLog()">Clear</button>
</header>
<div id="log"></div>
<footer>
  <span id="footer-count">0 lines</span>
  <span id="footer-status">Connecting…</span>
</footer>
<script>
  const logDiv   = document.getElementById('log');
  const indicator= document.getElementById('indicator');
  const footerCnt= document.getElementById('footer-count');
  const footerSt = document.getElementById('footer-status');
  let   lastCount= -1;

  function classifyLine(txt) {
    if (txt.includes('[RFID A]'))  return 'rfid-a';
    if (txt.includes('[RFID B]'))  return 'rfid-b';
    if (txt.includes('[HTTP]'))    return 'http';
    if (txt.includes('[SCAN]'))    return 'scan';
    if (txt.includes('[WiFi]'))    return 'wifi';
    if (txt.includes('[READY]'))   return 'ready';
    if (txt.toUpperCase().includes('ERROR') ||
        txt.toUpperCase().includes('NOT FOUND') ||
        txt.includes('ERR')) return 'error';
    if (txt.startsWith('=') || txt.trim() === '') return 'dim';
    return '';
  }

  function renderLines(lines) {
    lines.forEach(txt => {
      const el = document.createElement('div');
      el.className = 'line ' + classifyLine(txt);
      el.textContent = txt;
      logDiv.appendChild(el);
    });
    footerCnt.textContent = logDiv.children.length + ' lines';
    if (document.getElementById('autoscroll').checked) {
      logDiv.scrollTop = logDiv.scrollHeight;
    }
  }

  async function fetchLog() {
    try {
      const r = await fetch('/log');
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const data = await r.json();
      indicator.className = 'live';
      footerSt.textContent = 'Live — polling every )rawhtml" + String(LOG_POLL_MS) + R"rawhtml(ms';

      if (data.count !== lastCount) {
        logDiv.innerHTML = '';
        renderLines(data.lines);
        lastCount = data.count;
      }
    } catch(e) {
      indicator.className = 'err';
      footerSt.textContent = 'Error: ' + e.message;
    }
  }

  async function clearLog() {
    await fetch('/log/clear');
    logDiv.innerHTML = '';
    lastCount = -1;
    footerCnt.textContent = '0 lines';
  }

  fetchLog();
  setInterval(fetchLog, )rawhtml" + String(LOG_POLL_MS) + R"rawhtml();
</script>
</body>
</html>)rawhtml";

  server.send(200, "text/html", html);
}

void handleLog() {
  String json = "{\"count\":" + String(logCount) + ",\"lines\":[";

  int start = (logCount < LOG_BUFFER_LINES)
              ? 0
              : logHead;          

  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_BUFFER_LINES;
    
    String line = logBuffer[idx];
    line.replace("\\", "\\\\"); 
    line.replace("\"", "\\\"");
    line.replace("\n", "\\n");  
    line.replace("\r", "\\r");  
    line.replace("\t", "\\t");  
    
    if (i > 0) json += ",";
    json += "\"" + line + "\"";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleLogClear() {
  logHead  = 0;
  logCount = 0;
  server.send(200, "application/json", "{\"status\":\"cleared\"}");
}

// ─────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial);

  webLogln("\n=========================================");
  webLogln("  AMS DUAL RFID → HOME ASSISTANT        ");
  webLogln("=========================================");

  pinMode(MFRC522_CS, OUTPUT); digitalWrite(MFRC522_CS, HIGH);
  pinMode(PN532_CS,   OUTPUT); digitalWrite(PN532_CS,   HIGH);
  delay(10);

  webLog("[WiFi] Connecting to ");
  webLogln(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");  
  }
  webLogln("\n[WiFi] Connected. IP: " + WiFi.localIP().toString());

  // NEW: Initialize ArduinoOTA
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) type = "sketch";
    else type = "filesystem";
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  webLogln("[OTA] Over-the-Air updates active");

  server.on("/",          handleRoot);
  server.on("/scan",      handleScan);
  server.on("/status",    handleStatus);
  server.on("/log",       handleLog);        
  server.on("/log/clear", handleLogClear);   
  server.begin();
  webLogln("[HTTP] Server started on port 80");

  SPI.begin();
  mfrc522.PCD_Init();
  webLogln("[RFID A] MFRC522 ready");

  webLogln("[RFID B] Waking up PN532...");
  pn532.begin();
  uint32_t versiondata = pn532.getFirmwareVersion();
  if (!versiondata) {
    webLogln("[RFID B] PN532 NOT FOUND — check wiring & DIP switches (SW1=OFF, SW2=ON)");
  } else {
    webLog("[RFID B] Found chip PN5");
    webLogln((versiondata >> 24) & 0xFF, HEX);
    webLog("[RFID B] Firmware ");
    webLog((versiondata >> 16) & 0xFF, DEC);
    webLog(".");
    webLogln((versiondata >> 8) & 0xFF, DEC);
    pn532.SAMConfig();
    webLogln("[RFID B] PN532 ready");
  }

  webLogln("\n[READY] Waiting for HA scan window...");
}

// ─────────────────────────────────────────────────────────────
// Loop
// ─────────────────────────────────────────────────────────────
void loop() {
  // NEW: Process any incoming OTA update requests
  ArduinoOTA.handle();

  server.handleClient();

  if (activeReader == 0) return;

  if (millis() - scanStart > SCAN_WINDOW_MS) {
    webLogln("[SCAN] Window expired — no valid tag read. Notifying HA...");
    postTagToHA("TIMEOUT", activeReader);
    activeReader = 0;
    return;
  }

  // ── Reader A: MFRC522 (trays 1 & 2) ──────────────────────
  if (activeReader == 1) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = mfrcUidStr(mfrc522.uid);
      
      if (uid == ignoreUid) {
        // Halt it and ignore
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
      } else {
        webLogln("[RFID A] Tag read: " + uid);
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();

        postTagToHA(uid, 1);
        activeReader = 0;     
      }
    }
    return;
  }

  // ── Reader B: PN532 (trays 3 & 4) ────────────────────────
  if (activeReader == 2) {
    uint8_t uid[7];
    uint8_t uidLen = 0;

    bool found = pn532.readPassiveTargetID(
        PN532_MIFARE_ISO14443A, uid, &uidLen, PN532_POLL_MS);

    if (found && uidLen > 0) {
      String uidStr = pn532UidStr(uid, uidLen);
      
      if (uidStr == ignoreUid) {
         // Silently loop again until the scan window expires or a new tag is found
      } else {
        webLogln("[RFID B] Tag read: " + uidStr);
        postTagToHA(uidStr, 2);
        activeReader = 0;     
      }
    }
    return;
  }
}