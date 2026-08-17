/*
 * ALTGEO GSM tracker for LILYGO T-Call A7670 (ESP32 + SIMCom A7670 modem)
 * -----------------------------------------------------------------------------
 * Fully autonomous by design: plug in power (with a SIM card inserted), and
 * that's the entire setup. There is no config file, no SD card, no button to
 * press, no first-run wizard. Everything the device needs to talk to the
 * server (server address, shared sync password) is compiled into the
 * firmware itself (see CONFIGURE BEFORE FLASHING below) - the only thing
 * that's genuinely per-device is its IMEI, which the SIM/modem hardware
 * already provides on its own.
 *
 * Finding the device's IMEI (for registering it on your ALTGEO account):
 * the device continuously advertises over BLE as "ALTGEO_IMEI_<15 digits>" -
 * open Bluetooth settings on any phone nearby and it'll show up in the
 * scan list under exactly that name. No app needed, nothing to pair - this
 * is a broadcast-only BLE advertisement, not a pairable device. Copy the
 * digits after "ALTGEO_IMEI_" into the "Add device" form on your dashboard
 * (as an IMEI, not a chip ID).
 *
 * Positioning: GPS/GNSS is built into the A7670 modem itself (no separate
 * GPS chip on this board) - read over the same AT-command link as the
 * cellular functions, via TinyGSM's modem.getGPS(). WiFi networks are
 * gathered by a passive scan (never associates/connects to anything found -
 * same philosophy as the SD-card tracker). BLE sightings of OTHER nearby
 * devices are gathered by a short passive scan too, interleaved with this
 * device's own continuous advertising (the ESP32's BLE radio can scan and
 * advertise at the same time - this is a standard, well-supported combined
 * role, not a hack).
 *
 * Real-time sync with no data loss on GSM dropout: every captured point is
 * first appended to a small persistent queue on internal flash (LittleFS -
 * no SD card, per the "fully self-contained" hardware requirement). A
 * separate timer periodically tries to flush that queue to the server over
 * the cellular connection (POST /api/device/points, see the server's own
 * docs) - a point is only ever removed from the local queue once the server
 * has explicitly acknowledged storing it (by sequence number). If there's no
 * signal, or the request fails, or the server is unreachable, the queue
 * simply keeps growing (capped - oldest points are dropped first once the
 * flash budget is exhausted, exactly like the SD-tracker's circular buffer)
 * and gets flushed whenever connectivity comes back. Sequence numbers are
 * persisted in NVS (Preferences) so they stay monotonically increasing
 * across reboots/power loss too - the server's idempotency check depends on
 * that; see db.ingest_realtime_points in the server's own source for why.
 *
 * Hardware: LILYGO T-Call A7670 V1.0/V1.1 (ESP32 + SIMCom A7670 4G Cat-1
 * modem with integrated GNSS). Pin assignments below are sourced from
 * LilyGO's own reference firmware (LilyGO-T-A76XX examples/utilities.h) -
 * double-check against your specific board revision's silkscreen/schematic
 * before flashing a batch, since LilyGO has shipped more than one pin
 * layout under similar names over time.
 *
 * Library: TinyGSM (modem AT-command abstraction + built-in GNSS/IMEI
 * helpers), ArduinoHttpClient (HTTP over TinyGSM's TCP client),
 * ArduinoJson, and the ESP32 core's own WiFi/BLEDevice/LittleFS/Preferences.
 * Board (fqbn): esp32:esp32:esp32
 */

// ---------------------------------------------------------------------------
// CONFIGURE BEFORE FLASHING - the only things that differ between "your
// server" and anyone else's. Everything past this block is generic.
// ---------------------------------------------------------------------------
#define ALTGEO_SERVER_HOST   "mc.itprime.ru"   // no scheme, no path - just the host
#define ALTGEO_SERVER_PORT   80                // 80 for plain HTTP, 443 if you switch to TinyGsmClientSecure
#define ALTGEO_SERVER_PATH   "/api/device/points"
#define ALTGEO_SYNC_PASSWORD "407028109"       // must match the server's WIFIGPS_PASSWORD
// GSM APN - many SIMs/carriers accept a blank APN and get auto-assigned one
// by the network; if yours doesn't connect, put your carrier's APN here.
#define ALTGEO_APN           ""
#define ALTGEO_APN_USER      ""
#define ALTGEO_APN_PASS      ""

// ---------------------------------------------------------------------------
// Board pin + modem-model definitions - LILYGO T-Call A7670 V1.0/V1.1.
// Kept in a separate header (utilities.h) included FIRST, before
// <TinyGsmClient.h> - see that file for why this isn't just #define lines
// inline here (Arduino's prototype-generation pass can otherwise land
// between top-level #define/#include lines in the .ino itself, which is
// exactly what broke this the first time - see build log note in README).
// ---------------------------------------------------------------------------
#include "utilities.h"

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <LittleFS.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// General settings
// ---------------------------------------------------------------------------
#define POINT_INTERVAL_MS        30000UL   // capture one point every 30s
#define SYNC_INTERVAL_MS         20000UL   // try to flush the queue this often
#define SYNC_BATCH_SIZE          20        // at most this many points per HTTP POST
#define WIFI_SCAN_TIMEOUT_MS     4000UL
#define BLE_SCAN_SECONDS         2
#define QUEUE_MAX_BYTES          (1536UL * 1024UL)  // ~1.5MB flash budget for the queue file
#define GNSS_FIX_TIMEOUT_MS      5000UL
#define QUEUE_FILE               "/queue.jsonl"
#define QUEUE_TMP_FILE            "/queue.jsonl.tmp"

HardwareSerial modemSerial(1);
TinyGsm modem(modemSerial);
TinyGsmClient gsmClient(modem);
HttpClient http(gsmClient, ALTGEO_SERVER_HOST, ALTGEO_SERVER_PORT);

Preferences prefs;
String deviceImei = "";
uint32_t nextSeq = 1;

unsigned long lastPointMs = 0;
unsigned long lastSyncMs = 0;
bool gprsConnected = false;

int lastWifiCount = 0;
int lastBleCount = 0;
struct BleSighting { String mac; int rssi; };
#define MAX_BLE_SIGHTINGS 20
BleSighting bleSightings[MAX_BLE_SIGHTINGS];
int bleSightingCount = 0;

// ---------------------------------------------------------------------------
// LED heartbeat (the only feedback this device has - no screen). A slow
// blink means "running, no GSM yet"; a quick double-blink on each
// successful sync flash means "everything's working end to end".
// ---------------------------------------------------------------------------
static void ledPulse(int times, int onMs, int offMs) {
    for (int i = 0; i < times; i++) {
        digitalWrite(BOARD_LED_PIN, LED_ON);
        delay(onMs);
        digitalWrite(BOARD_LED_PIN, !LED_ON);
        if (i < times - 1) delay(offMs);
    }
}

// ---------------------------------------------------------------------------
// Modem power-on (SIMCom PWRKEY pulse sequence, per LilyGO's own reference
// firmware for this board) + TinyGSM init + GNSS enable.
// ---------------------------------------------------------------------------
static bool powerOnModem() {
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(1000);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(2000);

#ifdef MODEM_RESET_PIN
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
#endif
#ifdef MODEM_DTR_PIN
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);  // LOW = modem awake, not sleeping
#endif

    modemSerial.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(3000);

    Serial.println("Initializing modem...");
    if (!modem.init()) {
        Serial.println("Modem init failed - retrying with restart()...");
        if (!modem.restart()) {
            Serial.println("Modem restart also failed.");
            return false;
        }
    }
    Serial.print("Modem: "); Serial.println(modem.getModemInfo());
    return true;
}

static void enableGnss() {
    modem.enableGPS();
}

// ---------------------------------------------------------------------------
// IMEI + BLE advertising ("ALTGEO_IMEI_<imei>") - this is the device's only
// visible identity check; see the header comment for how a person uses it.
// ---------------------------------------------------------------------------
static void startImeiBleBroadcast() {
    deviceImei = modem.getIMEI();
    deviceImei.trim();
    Serial.print("IMEI: "); Serial.println(deviceImei);

    String bleName = "ALTGEO_IMEI_" + deviceImei;
    BLEDevice::init(bleName.c_str());
    BLEServer *server = BLEDevice::createServer();
    (void)server;  // no GATT services needed - the advertised name alone is the point
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->setScanResponse(true);
    // Keep advertising continuously, indefinitely - this runs on its own in
    // the BLE stack's background task and doesn't need to be "pumped" from
    // loop(), and doesn't stop the same radio from also scanning below.
    BLEDevice::startAdvertising();
}

// ---------------------------------------------------------------------------
// Persistent point queue on LittleFS (JSON-Lines: one JSON object per line,
// simple to append/parse/compact, robust to a partial write on power loss -
// worst case a single trailing malformed line, which is just skipped).
// ---------------------------------------------------------------------------
static bool initQueueStorage() {
    if (!LittleFS.begin(true)) {  // true = format on first-ever boot if needed
        Serial.println("LittleFS mount failed!");
        return false;
    }
    return true;
}

static void enforceQueueBudget() {
    File f = LittleFS.open(QUEUE_FILE, "r");
    if (!f) return;
    size_t size = f.size();
    f.close();
    if (size <= QUEUE_MAX_BYTES) return;

    // Over budget: drop the OLDEST entries (circular-buffer philosophy,
    // matching the SD-card tracker) by keeping only the tail of the file.
    File in = LittleFS.open(QUEUE_FILE, "r");
    File out = LittleFS.open(QUEUE_TMP_FILE, "w");
    if (!in || !out) { if (in) in.close(); if (out) out.close(); return; }

    size_t skipBytes = size - (QUEUE_MAX_BYTES * 3 / 4);  // trim back to 75% of budget
    in.seek(skipBytes);
    // Discard a possibly-partial first line after the seek.
    if (skipBytes > 0) in.readStringUntil('\n');
    while (in.available()) {
        out.print(in.readStringUntil('\n'));
        out.print('\n');
    }
    in.close();
    out.close();
    LittleFS.remove(QUEUE_FILE);
    LittleFS.rename(QUEUE_TMP_FILE, QUEUE_FILE);
    Serial.println("Queue over budget - oldest points dropped.");
}

static void appendPointToQueue(const String &jsonLine) {
    File f = LittleFS.open(QUEUE_FILE, "a");
    if (!f) { Serial.println("Failed to open queue file for append!"); return; }
    f.print(jsonLine);
    f.print('\n');
    f.close();
    enforceQueueBudget();
}

// ---------------------------------------------------------------------------
// WiFi + BLE passive scanning (never associates/connects/pairs - listen only)
// ---------------------------------------------------------------------------
static String scanWifiToJsonArray() {
    int n = WiFi.scanNetworks(false, false, false, (uint32_t)WIFI_SCAN_TIMEOUT_MS);
    String out = "[";
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (i > 0) out += ",";
            out += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"bssid\":\"" + WiFi.BSSIDstr(i) +
                   "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
        }
        lastWifiCount = n;
    } else {
        lastWifiCount = 0;
    }
    out += "]";
    WiFi.scanDelete();
    return out;
}

static void onBleDeviceFound(BLEAdvertisedDevice advertisedDevice) {
    if (bleSightingCount >= MAX_BLE_SIGHTINGS) return;
    String mac = advertisedDevice.getAddress().toString().c_str();
    for (int i = 0; i < bleSightingCount; i++) {
        if (bleSightings[i].mac == mac) {
            if (advertisedDevice.getRSSI() > bleSightings[i].rssi) {
                bleSightings[i].rssi = advertisedDevice.getRSSI();
            }
            return;
        }
    }
    bleSightings[bleSightingCount].mac = mac;
    bleSightings[bleSightingCount].rssi = advertisedDevice.getRSSI();
    bleSightingCount++;
}

class AltgeoBleCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        onBleDeviceFound(advertisedDevice);
    }
};

static String scanBleToJsonArray() {
    bleSightingCount = 0;
    BLEScan *scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(new AltgeoBleCallbacks(), true);
    scan->setActiveScan(false);  // passive - never sends a scan request
    scan->start(BLE_SCAN_SECONDS, false);
    scan->clearResults();

    String out = "[";
    for (int i = 0; i < bleSightingCount; i++) {
        if (i > 0) out += ",";
        out += "{\"mac\":\"" + bleSightings[i].mac + "\",\"rssi\":" + String(bleSightings[i].rssi) + "}";
    }
    out += "]";
    lastBleCount = bleSightingCount;
    return out;
}

// ---------------------------------------------------------------------------
// GNSS fix -> one queued point (status "ok" or "bad_gps", same convention as
// the SD-card tracker's log format)
// ---------------------------------------------------------------------------
static void captureAndQueuePoint() {
    float lat = 0, lon = 0, speed = 0, alt = 0;
    int vsat = 0, usat = 0;
    bool haveFix = modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat);

    String wifiJson = scanWifiToJsonArray();
    String bleJson = scanBleToJsonArray();

    uint32_t seq = nextSeq++;
    prefs.putUInt("nextSeq", nextSeq);  // persisted immediately - never lose seq continuity to a crash

    time_t nowSecs;
    struct tm tmStruct;
    char tsBuf[32];
    if (modem.getNetworkTime(&tmStruct.tm_year, &tmStruct.tm_mon, &tmStruct.tm_mday,
                              &tmStruct.tm_hour, &tmStruct.tm_min, &tmStruct.tm_sec, nullptr)) {
        snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
                 tmStruct.tm_year, tmStruct.tm_mon, tmStruct.tm_mday,
                 tmStruct.tm_hour, tmStruct.tm_min, tmStruct.tm_sec);
    } else {
        // No network time available yet - fall back to device uptime; the
        // server will still accept and store the point, just with a less
        // meaningful timestamp until network time is available.
        snprintf(tsBuf, sizeof(tsBuf), "uptime_%lu", millis() / 1000UL);
    }

    String line = "{\"seq\":" + String(seq) + ",\"ts\":\"" + String(tsBuf) + "\",";
    if (haveFix) {
        line += "\"lat\":" + String(lat, 6) + ",\"lon\":" + String(lon, 6) + ",\"status\":\"ok\",";
    } else {
        line += "\"lat\":null,\"lon\":null,\"status\":\"bad_gps\",";
    }
    line += "\"wifi\":" + wifiJson + ",\"ble\":" + bleJson + "}";

    appendPointToQueue(line);
    Serial.printf("Queued point seq=%lu fix=%d wifi=%d ble=%d\n",
                  (unsigned long)seq, haveFix, lastWifiCount, lastBleCount);
}

// ---------------------------------------------------------------------------
// GPRS connectivity + flushing the queue to the server
// ---------------------------------------------------------------------------
static bool ensureGprsConnected() {
    if (modem.isGprsConnected()) return true;
    Serial.println("Connecting GPRS...");
    if (!modem.waitForNetwork(15000L)) {
        Serial.println("No cellular network registration yet.");
        return false;
    }
    if (!modem.gprsConnect(ALTGEO_APN, ALTGEO_APN_USER, ALTGEO_APN_PASS)) {
        Serial.println("GPRS connect failed.");
        return false;
    }
    Serial.println("GPRS connected.");
    return true;
}

// Reads up to SYNC_BATCH_SIZE lines from the queue file. Returns the JSON
// body to POST and, via outSeqs, which seq numbers it contains (so the
// caller can remove exactly those once acknowledged).
static String buildSyncBatch(uint32_t outSeqs[], int &outCount) {
    outCount = 0;
    File f = LittleFS.open(QUEUE_FILE, "r");
    if (!f) return "";

    String body = "{\"points\":[";
    bool first = true;
    while (f.available() && outCount < SYNC_BATCH_SIZE) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        // Pull just the seq number back out for bookkeeping (cheap - avoids
        // a second full JSON parse of something we already have as text).
        int seqIdx = line.indexOf("\"seq\":");
        if (seqIdx < 0) continue;
        uint32_t seq = (uint32_t)line.substring(seqIdx + 6).toInt();
        outSeqs[outCount++] = seq;

        if (!first) body += ",";
        body += line;
        first = false;
    }
    f.close();
    body += "]}";
    return body;
}

// Removes queued lines whose seq is in ackedSeqs[0..ackedCount) - rewrites
// the file keeping only what's left, same rename-over pattern as
// enforceQueueBudget uses.
static void removeAckedFromQueue(uint32_t ackedSeqs[], int ackedCount) {
    if (ackedCount == 0) return;
    File in = LittleFS.open(QUEUE_FILE, "r");
    File out = LittleFS.open(QUEUE_TMP_FILE, "w");
    if (!in || !out) { if (in) in.close(); if (out) out.close(); return; }

    while (in.available()) {
        String line = in.readStringUntil('\n');
        String trimmed = line;
        trimmed.trim();
        if (trimmed.length() == 0) continue;

        int seqIdx = trimmed.indexOf("\"seq\":");
        uint32_t seq = seqIdx >= 0 ? (uint32_t)trimmed.substring(seqIdx + 6).toInt() : 0;

        bool acked = false;
        for (int i = 0; i < ackedCount; i++) {
            if (ackedSeqs[i] == seq) { acked = true; break; }
        }
        if (!acked) {
            out.print(trimmed);
            out.print('\n');
        }
    }
    in.close();
    out.close();
    LittleFS.remove(QUEUE_FILE);
    LittleFS.rename(QUEUE_TMP_FILE, QUEUE_FILE);
}

static void trySyncQueue() {
    File check = LittleFS.open(QUEUE_FILE, "r");
    if (!check || check.size() == 0) { if (check) check.close(); return; }
    check.close();

    if (!ensureGprsConnected()) return;

    uint32_t batchSeqs[SYNC_BATCH_SIZE];
    int batchCount = 0;
    String body = buildSyncBatch(batchSeqs, batchCount);
    if (batchCount == 0) return;

    http.beginRequest();
    http.post(ALTGEO_SERVER_PATH);
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("X-Device-IMEI", deviceImei);
    http.sendHeader("X-Sync-Password", ALTGEO_SYNC_PASSWORD);
    http.sendHeader("Content-Length", body.length());
    http.beginBody();
    http.print(body);
    http.endRequest();

    int status = http.responseStatusCode();
    String response = http.responseBody();
    http.stop();

    if (status != 200) {
        Serial.printf("Sync failed, HTTP %d - will retry next cycle.\n", status);
        return;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
        Serial.println("Sync response was not valid JSON - leaving queue as-is.");
        return;
    }

    JsonArray ackedArr = doc["acked_seqs"].as<JsonArray>();
    uint32_t ackedSeqs[SYNC_BATCH_SIZE];
    int ackedCount = 0;
    for (JsonVariant v : ackedArr) {
        if (ackedCount < SYNC_BATCH_SIZE) ackedSeqs[ackedCount++] = v.as<uint32_t>();
    }
    removeAckedFromQueue(ackedSeqs, ackedCount);
    Serial.printf("Sync ok - %d point(s) acknowledged and cleared from queue.\n", ackedCount);
    ledPulse(2, 80, 80);
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nALTGEO GSM tracker starting...");

    pinMode(BOARD_LED_PIN, OUTPUT);
    digitalWrite(BOARD_LED_PIN, !LED_ON);

    prefs.begin("altgeo", false);
    nextSeq = prefs.getUInt("nextSeq", 1);

    if (!initQueueStorage()) {
        Serial.println("Cannot continue without local storage - halting.");
        while (true) { ledPulse(1, 60, 940); }
    }

    if (!powerOnModem()) {
        Serial.println("Modem did not respond - will keep retrying in the background.");
    }
    enableGnss();
    startImeiBleBroadcast();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    lastPointMs = millis();
    lastSyncMs = millis();
    Serial.println("Setup complete - running autonomously.");
}

void loop() {
    unsigned long now = millis();

    if (now - lastPointMs >= POINT_INTERVAL_MS) {
        lastPointMs = now;
        captureAndQueuePoint();
    }

    if (now - lastSyncMs >= SYNC_INTERVAL_MS) {
        lastSyncMs = now;
        trySyncQueue();
    }

    if (!modem.isGprsConnected() && !modem.isNetworkConnected()) {
        digitalWrite(BOARD_LED_PIN, (now / 1000) % 2 == 0 ? LED_ON : !LED_ON);  // slow blink = no signal yet
    } else {
        digitalWrite(BOARD_LED_PIN, !LED_ON);
    }

    delay(200);
}
