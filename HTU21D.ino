#include <Wire.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebOTA.h>
#include <WebServer.h>
#include "WebInterface.h"
#include <Adafruit_AHTX0.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include <stdarg.h>
#include <time.h>

#define AHT_SDA_PIN 1
#define AHT_SCL_PIN 3

#define BQ_SDA 33
#define BQ_SCL 13
#define BQ25895_ADDRESS 0x6A
#define ENABLE_BQ 1

#define DIAGNOSTIC_LED_PIN 14
#define DIAGNOSTIC_LED_COUNT 2

#define SERVICE_UUID              "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DEBUG_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define HISTORY_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"

NimBLECharacteristic *pCharacteristic = nullptr;
NimBLECharacteristic *pDebugCharacteristic = nullptr;
NimBLECharacteristic *pHistoryCharacteristic = nullptr;

bool historyStreamActive = false;
uint16_t historyStreamIndex = 0;
uint32_t historyStreamLogical = 0;
uint32_t historyStreamStride = 1;
uint32_t historyStreamOldest = 0;
bool historyStreamPersistent = false;
uint32_t historyStreamCount = 0;
bool historyStreamHeaderPending = false;
uint32_t historyStreamExpectedPoints = 0;
// Logicki indeks od kojeg pocinje "noviji" region (stride=1, pun detalj).
// Sve sto je ispod ovog indeksa ce biti downsampled historyStreamStride-om.
uint32_t historyStreamRecentStart = 0;
// Drzimo fajl otvoren tokom celog stream-a umesto da ga otvaramo/zatvaramo na svaka 2 poena.
File historyStreamFile;


bool deviceConnected = false;
bool otaServerActive = false;
bool debugHttpServerActive = false;
bool otaStartRequested = false;

const char OTA_TRIGGER_CHAR = 'U';
const char OTA_AP_SSID[] = "VoiceToysWS-OTA";
const char WIFI_HOSTNAME[] = "ws";
const uint8_t MAX_KNOWN_NETWORKS = 5;
const uint32_t WIFI_STORE_MAGIC = 0x57494649;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 12000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 60000;

struct KnownNetwork {
  char ssid[33];
  char password[65];
};

struct WifiStore {
  uint32_t magic;
  uint8_t count;
  KnownNetwork networks[MAX_KNOWN_NETWORKS];
};

WifiStore wifiStore = {};
bool wifiApFallbackActive = false;
bool networkReconfigurePending = false;
unsigned long networkReconfigureAtMs = 0;
unsigned long lastWifiAttemptMs = 0;

unsigned long zadnjeVremeCitanja = 0;
unsigned long intervalCitanjaMs = 500;
unsigned long lastAhtProbeMs = 0;
unsigned long lastBatteryReadMs = 0;
unsigned long lastStatusLogMs = 0;
unsigned long diagnosticLedPhaseMs = 0;

const unsigned long BATTERY_READ_INTERVAL_MS = 30000;
const unsigned long DIAGNOSTIC_LED_PERIOD_MS = 3000;
const unsigned long DIAGNOSTIC_LED_ON_MS = 120;
const uint8_t DIAGNOSTIC_LED_BRIGHTNESS = 8;

uint16_t i2cTimeoutMs = 50;
uint16_t bqAdcDelayMs = 120;

bool ahtPresent = false;
bool ahtReadingValid = false;
bool bqPresent = false;
bool bqReadHealthy = true;
bool diagnosticLedsEnabled = true;
bool diagnosticLedsLit = false;
bool clockWasValid = false;

String debugBuffer = "";
const size_t DEBUG_BUFFER_MAX = 4096;
WebServer debugServer(8081);
Preferences snapshotPreferences;
Preferences wifiPreferences;

Adafruit_NeoPixel diagnosticLeds(DIAGNOSTIC_LED_COUNT, DIAGNOSTIC_LED_PIN, NEO_GRB + NEO_KHZ800);

const uint8_t MAX_SNAPSHOTS = 24;
const uint32_t SNAPSHOT_MAGIC = 0x534E4150;

struct Snapshot {
  uint32_t id;
  char capturedAt[32];
  float temperature;
  float humidity;
  double latitude;
  double longitude;
  bool hasLocation;
  char comment[96];
};

struct SnapshotStore {
  uint32_t magic;
  uint32_t nextId;
  uint8_t count;
  Snapshot items[MAX_SNAPSHOTS];
};

SnapshotStore snapshotStore = {};

float lastTemperature = 0.0f;
float lastHumidity = 0.0f;
float lastBatteryV = 0.0f;
float lastRealFeel = 0.0f;
float lastAbsoluteHumidity = 0.0f;
float temperatureRate = 0.0f;
float humidityRate = 0.0f;
float realFeelRate = 0.0f;
float absoluteHumidityRate = 0.0f;
unsigned long lastValidReadingMs = 0;
float smoothedTemperature = 0.0f;
float smoothedHumidity = 0.0f;
float smoothedRealFeel = 0.0f;
float smoothedAbsoluteHumidity = 0.0f;

// RAM istorija: zadnjih 10 minuta pri rezoluciji od 500 ms.
const uint16_t HISTORY_SIZE = 1200;
struct HistoryPoint {
  uint32_t seconds;
  float temperature;
  float humidity;
  float realFeel;
  float absoluteHumidity;
};
HistoryPoint historyPoints[HISTORY_SIZE];
uint16_t historyCount = 0;
uint16_t historyHead = 0;

// Trajna istorija: 1 s rezolucija, kruzno do 24 sata (~1 MB).
// Poslednjih 10 minuta ostaje u RAM-u na punih 500 ms.
// Flash se ne pise svake sekunde: tacke se nakupljaju i upisuju odjednom.
const uint32_t PERSISTENT_HISTORY_MAGIC = 0x434C494D;
const uint16_t PERSISTENT_HISTORY_VERSION = 4;
const uint32_t PERSISTENT_HISTORY_CAPACITY = 24UL * 60UL * 60UL;
// Dovoljno veliko da pokrije i najduzi BLE history stream (fajl je zauzet za citanje dok stream traje,
// pa se upis odlaze do kraja stream-a — vidi flushPersistentHistoryBatch/historyStreamActive guard).
const uint16_t PERSISTENT_BATCH_SIZE = 1800;
const char PERSISTENT_HISTORY_PATH[] = "/climate-history.bin";

struct PersistentHistoryHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t pointSize;
  uint32_t capacity;
  uint32_t count;
  uint32_t head;
  uint32_t sequence;
};

struct PersistentHistoryPoint {
  uint32_t sequence;
  int16_t temperature100;
  int16_t humidity100;
  int16_t realFeel100;
  int16_t absoluteHumidity100;
};

PersistentHistoryHeader persistentHistory = {};
bool persistentHistoryReady = false;
PersistentHistoryPoint persistentBatch[PERSISTENT_BATCH_SIZE];
uint16_t persistentBatchCount = 0;
unsigned long persistentBatchStartMs = 0;
unsigned long lastPersistentSampleMs = 0;

Adafruit_AHTX0 aht;

void debugLogf(const char* fmt, ...);
void startWebOtaServer();
void setupDebugHttpServer();
void handleDebugRoot();
void handleDebugLogs();
void handleDebugStatus();
void handleHistory();
void handleHistoryInterpolate();
void handleDebugSet();
void handleSnapshots();
void handleSnapshotCreate();
void handleSnapshotComment();
void handleSnapshotDelete();
void handleNetworkStatus();
void handleNetworkScan();
void handleNetworkSave();
void handleNetworkDelete();
void handleDiagnosticLedSet();
void handleDebugWifiSet();
void setWifiEnabled(bool enabled);
void loadSnapshots();
void saveSnapshots();
void loadKnownNetworks();
void saveKnownNetworks();
bool connectKnownNetwork();
void startFallbackAp();
void configureNetwork();
void startNetworkDiscovery();
bool isClockValid();
uint32_t currentSampleTimestamp();
void updateClockState();
void appendJsonString(String &json, const char *value);

bool probeI2C(TwoWire &bus, uint8_t addr);
bool initAHT();
bool readAHT(float &temperature, float &humidity);
float calculateAbsoluteHumidity(float temperature, float humidity);
float calculateRealFeel(float temperature, float humidity);
void updateClimateMetrics(float temperature, float humidity);
bool initPersistentHistory();
void accumulatePersistentHistory(float temperature, float humidity, float realFeel, float absoluteHumidity);
bool flushPersistentHistoryBatch();
bool interpolatePersistentRange(uint32_t startEpoch, uint32_t endEpoch, uint32_t &patched);

void writeBQRegister(uint8_t reg, uint8_t value);
bool readBQRegisterWithMode(uint8_t reg, uint8_t &value, bool useRepeatedStart);
uint8_t readBQRegister(uint8_t reg);
void initBQ25895();
float getBatteryVoltage();
void dumpBQRegisters();
void sampleBattery(bool force = false);
void initDiagnosticLeds();
void updateDiagnosticLeds();
void startDiagnosticLedTask();
void setDiagnosticLedsOff();

float calculateAbsoluteHumidity(float temperature, float humidity) {
  float saturationHpa = 6.112f * expf((17.67f * temperature) / (temperature + 243.5f));
  float vaporHpa = saturationHpa * humidity / 100.0f;
  return 216.7f * vaporHpa / (273.15f + temperature);
}

float calculateRealFeel(float temperature, float humidity) {
  // Steadman apparent temperature without wind/radiation input.
  float vaporHpa = (humidity / 100.0f) * 6.105f * expf((17.27f * temperature) / (237.7f + temperature));
  return temperature + 0.33f * vaporHpa - 4.0f;
}

bool isClockValid() {
  return time(nullptr) >= 1700000000;
}

uint32_t currentSampleTimestamp() {
  time_t now = time(nullptr);
  return now >= 1700000000 ? (uint32_t)now : millis() / 1000;
}

void updateClockState() {
  bool valid = isClockValid();
  if (valid && !clockWasValid) {
    // Odbaci samo kratke uptime uzorke iz tekuceg boota; sacuvana epoch istorija ostaje.
    historyCount = 0;
    historyHead = 0;
    persistentBatchCount = 0;
    persistentBatchStartMs = millis();
    debugLogf("Vreme: NTP sinhronizovan, istorija sada koristi Unix vreme.");
  }
  clockWasValid = valid;
}

bool initPersistentHistory() {
  if (!SPIFFS.begin(true)) {
    debugLogf("Istorija: SPIFFS nije dostupan; nastavljam samo sa RAM istorijom.");
    return false;
  }

  File file = SPIFFS.open(PERSISTENT_HISTORY_PATH, FILE_READ);
  bool valid = false;
  if (file && file.size() >= sizeof(PersistentHistoryHeader)) {
    valid = file.read((uint8_t*)&persistentHistory, sizeof(persistentHistory)) == sizeof(persistentHistory) &&
            persistentHistory.magic == PERSISTENT_HISTORY_MAGIC &&
            persistentHistory.version == PERSISTENT_HISTORY_VERSION &&
            persistentHistory.pointSize == sizeof(PersistentHistoryPoint) &&
            persistentHistory.capacity == PERSISTENT_HISTORY_CAPACITY &&
            persistentHistory.count <= PERSISTENT_HISTORY_CAPACITY &&
            persistentHistory.head < PERSISTENT_HISTORY_CAPACITY;
  }
  if (file) file.close();

  if (!valid) {
    memset(&persistentHistory, 0, sizeof(persistentHistory));
    persistentHistory.magic = PERSISTENT_HISTORY_MAGIC;
    persistentHistory.version = PERSISTENT_HISTORY_VERSION;
    persistentHistory.pointSize = sizeof(PersistentHistoryPoint);
    persistentHistory.capacity = PERSISTENT_HISTORY_CAPACITY;

    file = SPIFFS.open(PERSISTENT_HISTORY_PATH, FILE_WRITE);
    if (!file || file.write((uint8_t*)&persistentHistory, sizeof(persistentHistory)) != sizeof(persistentHistory)) {
      if (file) file.close();
      debugLogf("Istorija: kreiranje SPIFFS fajla nije uspelo.");
      return false;
    }
    file.close();
  }

  persistentBatchStartMs = millis();
  lastPersistentSampleMs = 0;
  debugLogf("Istorija: SPIFFS spreman, %lu/%lu sekundnih tacaka (do 24h).",
            persistentHistory.count, persistentHistory.capacity);
  return true;
}

bool flushPersistentHistoryBatch() {
  if (!persistentHistoryReady || persistentBatchCount == 0) return true;
  // Ne otvaraj drugi handle dok BLE stream drzi fajl otvoren za citanje (SPIFFS ne podnosi dobro
  // dva istovremena handle-a na istom fajlu); sacekaj da se stream zavrsi, uzorci ostaju u RAM batch-u.
  if (historyStreamActive) return false;

  File file = SPIFFS.open(PERSISTENT_HISTORY_PATH, "r+");
  if (!file) return false;

  PersistentHistoryHeader nextHistory = persistentHistory;
  bool ok = true;
  for (uint16_t i = 0; i < persistentBatchCount && ok; i++) {
    size_t offset = sizeof(PersistentHistoryHeader) +
                    (size_t)nextHistory.head * sizeof(PersistentHistoryPoint);
    ok = file.seek(offset, SeekSet) &&
         file.write((const uint8_t*)&persistentBatch[i], sizeof(PersistentHistoryPoint)) ==
         sizeof(PersistentHistoryPoint);
    if (!ok) break;
    nextHistory.head = (nextHistory.head + 1) % nextHistory.capacity;
    if (nextHistory.count < nextHistory.capacity) nextHistory.count++;
    nextHistory.sequence = persistentBatch[i].sequence;
  }
  if (ok) {
    ok = file.seek(0, SeekSet) &&
         file.write((const uint8_t*)&nextHistory, sizeof(nextHistory)) == sizeof(nextHistory);
  }
  file.close();
  if (ok) {
    persistentHistory = nextHistory;
    persistentBatchCount = 0;
  }
  return ok;
}

// Binary search over stored epoch sequences (not the current clock) so delta-sync stays correct across reboots.
uint32_t findPersistentSkipCount(uint32_t sinceEpoch, uint32_t oldestPhysical, uint32_t count) {
  if (sinceEpoch == 0 || count == 0) return 0;
  File file = SPIFFS.open(PERSISTENT_HISTORY_PATH, FILE_READ);
  if (!file) return 0;
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint32_t physical = (oldestPhysical + mid) % persistentHistory.capacity;
    size_t offset = sizeof(PersistentHistoryHeader) + (size_t)physical * sizeof(PersistentHistoryPoint);
    PersistentHistoryPoint p;
    if (!file.seek(offset, SeekSet) || file.read((uint8_t*)&p, sizeof(p)) != sizeof(p)) break;
    if (p.sequence <= sinceEpoch) lo = mid + 1;
    else hi = mid;
  }
  file.close();
  return lo;
}

// Rewrites the stored range [startEpoch, endEpoch] with a linear ramp between the surrounding
// good samples, so a correction made in one client is visible to every other client.
bool interpolatePersistentRange(uint32_t startEpoch, uint32_t endEpoch, uint32_t &patched) {
  patched = 0;
  if (!persistentHistoryReady || persistentHistory.count == 0 || endEpoch < startEpoch) return false;
  if (!flushPersistentHistoryBatch()) return false;

  const uint32_t count = persistentHistory.count;
  const uint32_t oldest = (persistentHistory.head + persistentHistory.capacity - count) % persistentHistory.capacity;
  const uint32_t first = startEpoch == 0 ? 0 : findPersistentSkipCount(startEpoch - 1, oldest, count);
  const uint32_t afterLast = findPersistentSkipCount(endEpoch, oldest, count);
  if (afterLast <= first) return false;

  File file = SPIFFS.open(PERSISTENT_HISTORY_PATH, "r+");
  if (!file) return false;

  auto readAt = [&](uint32_t logical, PersistentHistoryPoint &p) {
    uint32_t physical = (oldest + logical) % persistentHistory.capacity;
    size_t offset = sizeof(PersistentHistoryHeader) + (size_t)physical * sizeof(PersistentHistoryPoint);
    return file.seek(offset, SeekSet) && file.read((uint8_t*)&p, sizeof(p)) == sizeof(p);
  };

  PersistentHistoryPoint left, right;
  bool hasLeft = first > 0 && readAt(first - 1, left);
  bool hasRight = afterLast < count && readAt(afterLast, right);
  if (!hasLeft && !hasRight) {
    file.close();
    return false;
  }
  if (!hasLeft) left = right;
  if (!hasRight) right = left;

  const float span = (float)right.sequence - (float)left.sequence;
  bool ok = true;
  for (uint32_t logical = first; logical < afterLast && ok; logical++) {
    PersistentHistoryPoint p;
    if (!readAt(logical, p)) { ok = false; break; }

    float frac = span <= 0.0f ? 0.0f : ((float)p.sequence - (float)left.sequence) / span;
    float t = (left.temperature100 + frac * (right.temperature100 - left.temperature100)) / 100.0f;
    float h = (left.humidity100 + frac * (right.humidity100 - left.humidity100)) / 100.0f;
    p.temperature100 = (int16_t)roundf(t * 100.0f);
    p.humidity100 = (int16_t)roundf(h * 100.0f);
    p.realFeel100 = (int16_t)roundf(calculateRealFeel(t, h) * 100.0f);
    p.absoluteHumidity100 = (int16_t)roundf(calculateAbsoluteHumidity(t, h) * 100.0f);

    uint32_t physical = (oldest + logical) % persistentHistory.capacity;
    size_t offset = sizeof(PersistentHistoryHeader) + (size_t)physical * sizeof(PersistentHistoryPoint);
    ok = file.seek(offset, SeekSet) &&
         file.write((const uint8_t*)&p, sizeof(p)) == sizeof(p);
    if (ok) patched++;
  }
  file.close();

  debugLogf("Istorija: interpolirano %lu tacaka (%lu-%lu).", patched, startEpoch, endEpoch);
  return ok && patched > 0;
}

void accumulatePersistentHistory(float temperature, float humidity, float realFeel, float absoluteHumidity) {
  if (!persistentHistoryReady || !isClockValid()) return;

  unsigned long now = millis();
  if (lastPersistentSampleMs != 0 && now - lastPersistentSampleMs < 1000) return;
  lastPersistentSampleMs = now;

  if (persistentBatchCount >= PERSISTENT_BATCH_SIZE && !flushPersistentHistoryBatch()) {
    debugLogf("Istorija: pun batch ceka ponovni SPIFFS upis.");
    return;
  }

  PersistentHistoryPoint &point = persistentBatch[persistentBatchCount++];
  point.sequence = currentSampleTimestamp();
  point.temperature100 = (int16_t)roundf(temperature * 100.0f);
  point.humidity100 = (int16_t)roundf(humidity * 100.0f);
  point.realFeel100 = (int16_t)roundf(realFeel * 100.0f);
  point.absoluteHumidity100 = (int16_t)roundf(absoluteHumidity * 100.0f);

  if ((persistentBatchCount >= PERSISTENT_BATCH_SIZE || now - persistentBatchStartMs >= 60000) &&
      !flushPersistentHistoryBatch()) {
    debugLogf("Istorija: SPIFFS upis nije uspeo.");
  }
  if (persistentBatchCount == 0) persistentBatchStartMs = now;
}

void updateClimateMetrics(float temperature, float humidity) {
  float realFeel = calculateRealFeel(temperature, humidity);
  float absoluteHumidity = calculateAbsoluteHumidity(temperature, humidity);
  unsigned long now = millis();

  if (lastValidReadingMs == 0) {
    smoothedTemperature = temperature;
    smoothedHumidity = humidity;
    smoothedRealFeel = realFeel;
    smoothedAbsoluteHumidity = absoluteHumidity;
  } else {
    // Blagi EMA filter: dovoljno brz, ali ignorise sitno podrhtavanje senzora.
    const float alpha = 0.25f;
    smoothedTemperature += alpha * (temperature - smoothedTemperature);
    smoothedHumidity += alpha * (humidity - smoothedHumidity);
    smoothedRealFeel += alpha * (realFeel - smoothedRealFeel);
    smoothedAbsoluteHumidity += alpha * (absoluteHumidity - smoothedAbsoluteHumidity);
  }

  // Trend i grafikoni rade na jednoj decimali; glavne brojke ostaju sirove.
  float chartTemperature = roundf(smoothedTemperature * 10.0f) / 10.0f;
  float chartHumidity = roundf(smoothedHumidity * 10.0f) / 10.0f;
  float chartRealFeel = roundf(smoothedRealFeel * 10.0f) / 10.0f;
  float chartAbsoluteHumidity = roundf(smoothedAbsoluteHumidity * 10.0f) / 10.0f;

  if (lastValidReadingMs != 0 && now > lastValidReadingMs && historyCount > 0) {
    uint8_t window = historyCount < 5 ? historyCount : 5;
    uint8_t previousIndex = (historyHead + HISTORY_SIZE - window) % HISTORY_SIZE;
    const HistoryPoint &previous = historyPoints[previousIndex];
    float elapsedSeconds = (now / 1000.0f) - previous.seconds;
    float minutes = elapsedSeconds > 0.1f ? elapsedSeconds / 60.0f : 1.0f / 60.0f;
    float previousTemperature = roundf(previous.temperature * 10.0f) / 10.0f;
    float previousHumidity = roundf(previous.humidity * 10.0f) / 10.0f;
    float previousRealFeel = roundf(previous.realFeel * 10.0f) / 10.0f;
    float previousAbsoluteHumidity = roundf(previous.absoluteHumidity * 10.0f) / 10.0f;
    temperatureRate = chartTemperature == previousTemperature ? 0.0f : (chartTemperature - previousTemperature) / minutes;
    humidityRate = chartHumidity == previousHumidity ? 0.0f : (chartHumidity - previousHumidity) / minutes;
    realFeelRate = chartRealFeel == previousRealFeel ? 0.0f : (chartRealFeel - previousRealFeel) / minutes;
    absoluteHumidityRate = chartAbsoluteHumidity == previousAbsoluteHumidity ? 0.0f : (chartAbsoluteHumidity - previousAbsoluteHumidity) / minutes;
  }

  lastTemperature = temperature;
  lastHumidity = humidity;
  lastRealFeel = realFeel;
  lastAbsoluteHumidity = absoluteHumidity;
  lastValidReadingMs = now;

  accumulatePersistentHistory(smoothedTemperature, smoothedHumidity,
                              smoothedRealFeel, smoothedAbsoluteHumidity);

  HistoryPoint &point = historyPoints[historyHead];
  point.seconds = currentSampleTimestamp();
  point.temperature = smoothedTemperature;
  point.humidity = smoothedHumidity;
  point.realFeel = smoothedRealFeel;
  point.absoluteHumidity = smoothedAbsoluteHumidity;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

void debugLogf(const char* fmt, ...) {
  char message[180];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  char lineBuffer[220];
  snprintf(lineBuffer, sizeof(lineBuffer), "[%10lu ms] %s", millis(), message);
  String line = String(lineBuffer);

  debugBuffer += line + "\n";
  if (debugBuffer.length() > DEBUG_BUFFER_MAX) {
    debugBuffer = debugBuffer.substring(debugBuffer.length() - DEBUG_BUFFER_MAX);
  }

  if (pDebugCharacteristic == nullptr) {
    return;
  }

  pDebugCharacteristic->setValue((uint8_t*)debugBuffer.c_str(), debugBuffer.length());

  if (deviceConnected) {
    size_t len = line.length();
    for (size_t off = 0; off < len; off += 20) {
      size_t chunk = (len - off) < 20 ? (len - off) : 20;
      pDebugCharacteristic->setValue((uint8_t*)(line.c_str() + off), chunk);
      pDebugCharacteristic->notify();
      delay(10);
    }
    pDebugCharacteristic->setValue((uint8_t*)debugBuffer.c_str(), debugBuffer.length());
  }
}


class MyHistoryCallbacks: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    std::string value = pChar->getValue();
    if (value.empty()) return;
    if (value[0] == 'I' && value.size() >= 9) { // "I" + start epoch + end epoch (both 4 LE bytes)
      auto le32 = [&](size_t i) {
        return (uint32_t)(uint8_t)value[i] |
               ((uint32_t)(uint8_t)value[i + 1] << 8) |
               ((uint32_t)(uint8_t)value[i + 2] << 16) |
               ((uint32_t)(uint8_t)value[i + 3] << 24);
      };
      uint32_t patched = 0;
      interpolatePersistentRange(le32(1), le32(5), patched);
      return;
    }
    if (value[0] == 'G') { // "G" + optional 4 LE bytes: epoch to sync since (0 or absent = full history)
      uint32_t sinceEpoch = 0;
      if (value.size() >= 5) {
        sinceEpoch = (uint32_t)(uint8_t)value[1] |
                     ((uint32_t)(uint8_t)value[2] << 8) |
                     ((uint32_t)(uint8_t)value[3] << 16) |
                     ((uint32_t)(uint8_t)value[4] << 24);
      }
      uint32_t rangeSeconds = 86400;
      historyStreamActive = true;
      historyStreamIndex = 0;
      historyStreamLogical = 0;
      historyStreamRecentStart = 0;
      uint32_t remainingPoints = 0;

      // Tacke u poslednjih RECENT_FULL_RES_SECONDS sekundi uvek idu na punoj rezoluciji.
      // Stariji podaci se downsampluju da ukupni broj prenetih tacaka ostane razuman.
      const uint32_t RECENT_FULL_RES_SECONDS = 7200; // 2 sata
      const uint32_t OLD_MAX_POINTS          = 400;  // max tacaka za stari region

      if (!persistentHistoryReady || persistentHistory.count == 0) {
        historyStreamPersistent = false;
        historyStreamCount = historyCount;
        uint16_t skip = 0;
        if (sinceEpoch > 0) {
          uint16_t oldestIdx = (historyHead + HISTORY_SIZE - historyCount) % HISTORY_SIZE;
          while (skip < historyCount && historyPoints[(oldestIdx + skip) % HISTORY_SIZE].seconds <= sinceEpoch) {
            skip++;
          }
        }
        historyStreamIndex = skip;
        remainingPoints = historyCount - skip;
        // RAM istorija je max 10 minuta — uvek je "nova", nema potrebbe za downsampling.
        historyStreamStride = 1;
        historyStreamRecentStart = skip; // sve je recent
      } else {
        if (historyStreamFile) historyStreamFile.close();
        historyStreamFile = SPIFFS.open(PERSISTENT_HISTORY_PATH, FILE_READ);
        historyStreamPersistent = true;
        historyStreamOldest = (persistentHistory.head + persistentHistory.capacity - persistentHistory.count) % persistentHistory.capacity;
        uint32_t skip = sinceEpoch > 0
          ? findPersistentSkipCount(sinceEpoch, historyStreamOldest, persistentHistory.count)
          : (persistentHistory.count > rangeSeconds ? persistentHistory.count - rangeSeconds : 0);
        uint32_t req = persistentHistory.count - skip;
        remainingPoints = req;

        // Izracunaj gde pocinje "novi" region (poslednjih RECENT_FULL_RES_SECONDS sekundi).
        // Trajnna istorija je na 1 Hz, pa broj tacaka == sekunde trajanja.
        uint32_t recentPointCount = min((uint32_t)RECENT_FULL_RES_SECONDS, persistentHistory.count);
        uint32_t recentStartLogical = persistentHistory.count > recentPointCount
                                    ? persistentHistory.count - recentPointCount
                                    : 0;
        // Ako je skip veci od recentStartLogical, sve preostale tacke su vec "nove".
        historyStreamRecentStart = recentStartLogical > skip ? recentStartLogical : skip;

        uint32_t oldCount = historyStreamRecentStart - skip;
        uint32_t recentCount = persistentHistory.count - historyStreamRecentStart;

        // Stride se primenjuje samo na stari region.
        historyStreamStride = oldCount > OLD_MAX_POINTS ? (oldCount + OLD_MAX_POINTS - 1) / OLD_MAX_POINTS : 1;

        historyStreamLogical = skip;
        historyStreamCount = persistentHistory.count;

        // Ocekivani broj tacaka = downsampled stari deo + pun novi deo
        uint32_t sampledOld = historyStreamStride > 0 ? (oldCount + historyStreamStride - 1) / historyStreamStride : 0;
        remainingPoints = sampledOld + recentCount;
      }

      historyStreamExpectedPoints = remainingPoints;
      historyStreamHeaderPending = true;
      debugLogf("BLE History Stream pokrenut (since=%lu, %lu tacaka, stride=%lu, recentStart=%lu).",
                sinceEpoch, historyStreamExpectedPoints, historyStreamStride, historyStreamRecentStart);
    }
  }
};


class MyServerCallbacks: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
    deviceConnected = true;
    debugLogf("Klijent spojen.");
    pServer->updateConnParams(desc->conn_handle, 24, 40, 0, 200);
  }

  void onDisconnect(NimBLEServer* pServer) override {
    deviceConnected = false;
    historyStreamActive = false;
    historyStreamHeaderPending = false;
    if (historyStreamFile) historyStreamFile.close();
    debugLogf("Klijent otkacen. Ponovno oglasavanje...");
    pServer->getAdvertising()->start();
  }
};

class MyCharacteristicCallbacks: public NimBLECharacteristicCallbacks {
  void handleWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    if (value.empty()) {
      return;
    }

    if (value[0] == OTA_TRIGGER_CHAR) {
      otaStartRequested = true;
      debugLogf("BLE komanda primljena: pokretanje OTA servera.");
    } else if (value[0] == 'W' && value.size() >= 2) {
      setWifiEnabled(value[1] != 0);
    } else if (value[0] == 'T' && value.size() >= 5) {
      // Primanje apsolutnog Unix vremena sa mobilnog telefona za instant RTC sinkronizaciju preko BLE.
      uint32_t epoch = (uint32_t)(uint8_t)value[1] |
                       ((uint32_t)(uint8_t)value[2] << 8) |
                       ((uint32_t)(uint8_t)value[3] << 16) |
                       ((uint32_t)(uint8_t)value[4] << 24);
      struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
      settimeofday(&tv, NULL);
      updateClockState();
      debugLogf("Vreme sinhronizovano preko BLE: %lu", (unsigned long)epoch);
    }
  }

  void onWrite(NimBLECharacteristic* pCharacteristic) {
    handleWrite(pCharacteristic);
  }

  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    handleWrite(pCharacteristic);
  }
};

bool probeI2C(TwoWire &bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

bool initAHT() {
  pinMode(AHT_SDA_PIN, INPUT_PULLUP);
  pinMode(AHT_SCL_PIN, INPUT_PULLUP);

  Wire1.begin(AHT_SDA_PIN, AHT_SCL_PIN);
  Wire1.setClock(100000);
  Wire1.setTimeOut(i2cTimeoutMs);
  delay(20);

  if (!aht.begin(&Wire1)) {
    ahtPresent = false;
    ahtReadingValid = false;
    debugLogf("AHTX0 nije pronadjen (SDA=%d SCL=%d).", AHT_SDA_PIN, AHT_SCL_PIN);
    return false;
  }

  ahtPresent = true;
  debugLogf("AHTX0 spreman (Adafruit), SDA=%d SCL=%d.", AHT_SDA_PIN, AHT_SCL_PIN);
  return true;
}

bool readAHT(float &temperature, float &humidity) {
  if (!ahtPresent) {
    return false;
  }

  sensors_event_t humidityEvent;
  sensors_event_t tempEvent;
  aht.getEvent(&humidityEvent, &tempEvent);

  if (isnan(tempEvent.temperature) || isnan(humidityEvent.relative_humidity)) {
    return false;
  }

  temperature = tempEvent.temperature;
  humidity = humidityEvent.relative_humidity;
  return true;
}

void writeBQRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(BQ25895_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  uint8_t err = Wire.endTransmission(true);
  if (err != 0) {
    bqReadHealthy = false;
    debugLogf("BQ write fail REG%02X err=%u", reg, err);
  }
}

bool readBQRegisterWithMode(uint8_t reg, uint8_t &value, bool useRepeatedStart) {
  Wire.beginTransmission(BQ25895_ADDRESS);
  Wire.write(reg);
  uint8_t err = Wire.endTransmission(!useRepeatedStart);
  if (err != 0) {
    return false;
  }

  uint8_t got = Wire.requestFrom((uint8_t)BQ25895_ADDRESS, (uint8_t)1);
  if (got != 1 || Wire.available() != 1) {
    return false;
  }

  value = Wire.read();
  return true;
}

uint8_t readBQRegister(uint8_t reg) {
  uint8_t value = 0;

  if (readBQRegisterWithMode(reg, value, false)) {
    return value;
  }
  if (readBQRegisterWithMode(reg, value, true)) {
    return value;
  }

  bqReadHealthy = false;
  debugLogf("BQ read fail REG%02X", reg);
  return 0;
}

void initBQ25895() {
  uint8_t reg07 = readBQRegister(0x07);
  reg07 &= ~0x30;
  writeBQRegister(0x07, reg07);

  uint8_t reg03 = readBQRegister(0x03);
  reg03 |= (1 << 5);
  writeBQRegister(0x03, reg03);

  uint8_t reg0A = readBQRegister(0x0A);
  reg0A &= 0x03;
  reg0A |= (0x09 << 4);
  reg0A |= 0x01;
  writeBQRegister(0x0A, reg0A);

  debugLogf("BQ boost ON (REG03.5=1, REG0A=0x%02X)", readBQRegister(0x0A));

  for (int i = 0; i < 3; i++) {
    uint8_t reg02 = readBQRegister(0x02);
    reg02 &= ~(1 << 6);
    reg02 |= (1 << 7);
    writeBQRegister(0x02, reg02);
    delay(100);
  }
}

float getBatteryVoltage() {
  bqReadHealthy = true;
  uint8_t reg02 = readBQRegister(0x02);
  reg02 &= ~(1 << 6);
  reg02 |= (1 << 7);
  writeBQRegister(0x02, reg02);
  delay(bqAdcDelayMs);

  uint8_t reg0E = readBQRegister(0x0E);
  if (!bqReadHealthy) return NAN;
  return 2.304f + ((reg0E & 0x7F) * 0.020f);
}

void dumpBQRegisters() {
  const uint8_t regs[] = {0x00, 0x02, 0x03, 0x07, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F, 0x11, 0x14};
  for (uint8_t i = 0; i < sizeof(regs); i++) {
    debugLogf("BQ REG%02X = 0x%02X", regs[i], readBQRegister(regs[i]));
  }
}

void sampleBattery(bool force) {
#if ENABLE_BQ
  unsigned long now = millis();
  if (!force && now - lastBatteryReadMs < BATTERY_READ_INTERVAL_MS) return;
  lastBatteryReadMs = now;

  if (!bqPresent) {
    bqPresent = probeI2C(Wire, BQ25895_ADDRESS);
    if (!bqPresent) {
      bqReadHealthy = false;
      lastBatteryV = 0.0f;
      return;
    }
    initBQ25895();
  }

  float voltage = getBatteryVoltage();
  if (isnan(voltage)) {
    bqPresent = false;
    lastBatteryV = 0.0f;
    debugLogf("BQ: merenje napona nije uspelo.");
    return;
  }
  lastBatteryV = voltage;
  debugLogf("BQ: baterija %.2f V.", lastBatteryV);
#endif
}

void setDiagnosticLedsOff() {
  diagnosticLeds.clear();
  diagnosticLeds.show();
  diagnosticLedsLit = false;
}

void initDiagnosticLeds() {
  diagnosticLeds.begin();
  diagnosticLeds.setBrightness(DIAGNOSTIC_LED_BRIGHTNESS);
  setDiagnosticLedsOff();
  diagnosticLedPhaseMs = millis() - DIAGNOSTIC_LED_PERIOD_MS;
}

// Runs on its own core so LED timing stays precise regardless of how busy loop() gets
// (BLE streaming, WebOTA, debug HTTP server, etc. all run on the other core).
void diagnosticLedTask(void *parameter) {
  for (;;) {
    updateDiagnosticLeds();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void startDiagnosticLedTask() {
  xTaskCreatePinnedToCore(diagnosticLedTask, "DiagLED", 2048, NULL, 1, NULL, 0);
}

void updateDiagnosticLeds() {
  if (!diagnosticLedsEnabled) {
    if (diagnosticLedsLit) setDiagnosticLedsOff();
    return;
  }

  unsigned long elapsed = millis() - diagnosticLedPhaseMs;
  if (!diagnosticLedsLit && elapsed >= DIAGNOSTIC_LED_PERIOD_MS) {
    diagnosticLedPhaseMs = millis();
    uint32_t batteryColor;
    if (!bqPresent || !bqReadHealthy || lastBatteryV <= 0.0f) {
      batteryColor = diagnosticLeds.Color(255, 0, 0);
    } else if (lastBatteryV >= 3.75f) {
      batteryColor = diagnosticLeds.Color(0, 255, 0);
    } else if (lastBatteryV >= 3.45f) {
      batteryColor = diagnosticLeds.Color(255, 110, 0);
    } else {
      batteryColor = diagnosticLeds.Color(255, 0, 0);
    }
    diagnosticLeds.setPixelColor(0, batteryColor);
    diagnosticLeds.setPixelColor(1, ahtReadingValid ? diagnosticLeds.Color(0, 255, 0) : diagnosticLeds.Color(255, 0, 0));
    diagnosticLeds.show();
    diagnosticLedsLit = true;
  } else if (diagnosticLedsLit && elapsed >= DIAGNOSTIC_LED_ON_MS) {
    setDiagnosticLedsOff();
  }
}

void loadKnownNetworks() {
  wifiPreferences.begin("wifi-config", false);
  size_t storedSize = wifiPreferences.getBytesLength("networks");
  if (storedSize == sizeof(wifiStore)) {
    wifiPreferences.getBytes("networks", &wifiStore, sizeof(wifiStore));
  }
  if (wifiStore.magic != WIFI_STORE_MAGIC || wifiStore.count > MAX_KNOWN_NETWORKS) {
    memset(&wifiStore, 0, sizeof(wifiStore));
    wifiStore.magic = WIFI_STORE_MAGIC;
  }
  debugLogf("WiFi: ucitano %u/%u poznatih mreza.", wifiStore.count, MAX_KNOWN_NETWORKS);
}

void saveKnownNetworks() {
  wifiPreferences.putBytes("networks", &wifiStore, sizeof(wifiStore));
}

bool connectKnownNetwork() {
  if (wifiStore.count == 0) return false;

  if (wifiApFallbackActive) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
  }
  WiFi.setHostname(WIFI_HOSTNAME);

  debugLogf("WiFi: trazim dostupne poznate mreze...");
  int found = WiFi.scanNetworks();
  int selected = -1;
  int32_t selectedRssi = -1000;
  for (int scanIndex = 0; scanIndex < found; scanIndex++) {
    String scannedSsid = WiFi.SSID(scanIndex);
    for (uint8_t knownIndex = 0; knownIndex < wifiStore.count; knownIndex++) {
      if (scannedSsid == wifiStore.networks[knownIndex].ssid && WiFi.RSSI(scanIndex) > selectedRssi) {
        selected = knownIndex;
        selectedRssi = WiFi.RSSI(scanIndex);
      }
    }
  }
  WiFi.scanDelete();

  if (selected < 0) {
    debugLogf("WiFi: nijedna poznata mreza nije dostupna.");
    return false;
  }

  KnownNetwork &network = wifiStore.networks[selected];
  debugLogf("WiFi: povezivanje na '%s' (RSSI %ld dBm)...", network.ssid, selectedRssi);
  WiFi.begin(network.ssid, network.password);
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    debugLogf("WiFi: povezivanje na '%s' nije uspelo.", network.ssid);
    return false;
  }

  if (wifiApFallbackActive) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
  wifiApFallbackActive = false;
  debugLogf("WiFi: povezan na '%s', IP=%s.", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  return true;
}

void startFallbackAp() {
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(OTA_AP_SSID)) {
    debugLogf("WiFi: greska pri pokretanju fallback AP mreze.");
    return;
  }
  wifiApFallbackActive = true;
  debugLogf("WiFi: fallback AP '%s', IP=%s.", OTA_AP_SSID, WiFi.softAPIP().toString().c_str());
}

void configureNetwork() {
  lastWifiAttemptMs = millis();
  if (!connectKnownNetwork()) {
    startFallbackAp();
  }
}

void startNetworkDiscovery() {
  MDNS.end();
  if (MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", 8081);
    MDNS.addService("http", "tcp", 8080);
    debugLogf("WiFi: lokalna adresa http://%s.local:8081/", WIFI_HOSTNAME);
  } else {
    debugLogf("WiFi: mDNS ime nije pokrenuto; koristi prikazanu IP adresu.");
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    debugLogf("Vreme: NTP sinhronizacija pokrenuta.");
  }
}

void startWebOtaServer() {
  if (otaServerActive) {
    debugLogf("OTA server je vec aktivan.");
    return;
  }

  debugLogf("Pokrecem WiFi i WebOTA server...");
  configureNetwork();

  if (!webota.init(8080, "/webota")) {
    debugLogf("Greska pri pokretanju WebOTA servera.");
    return;
  }

  otaServerActive = true;
  setupDebugHttpServer();
  startNetworkDiscovery();
  IPAddress dashboardIp = wifiApFallbackActive ? WiFi.softAPIP() : WiFi.localIP();
  debugLogf("WebOTA spreman na portu 8080, putanja /webota.");
  debugLogf("Dashboard: http://%s:8081/", dashboardIp.toString().c_str());
}

void setupDebugHttpServer() {
  if (debugHttpServerActive) {
    return;
  }

  debugServer.on("/", HTTP_GET, handleDebugRoot);
  debugServer.on("/logs", HTTP_GET, handleDebugLogs);
  debugServer.on("/status", HTTP_GET, handleDebugStatus);
  debugServer.on("/history", HTTP_GET, handleHistory);
  debugServer.on("/history/interpolate", HTTP_POST, handleHistoryInterpolate);
  debugServer.on("/set", HTTP_GET, handleDebugSet);
  debugServer.on("/snapshots", HTTP_GET, handleSnapshots);
  debugServer.on("/snapshot", HTTP_POST, handleSnapshotCreate);
  debugServer.on("/snapshot/comment", HTTP_POST, handleSnapshotComment);
  debugServer.on("/snapshot/delete", HTTP_POST, handleSnapshotDelete);
  debugServer.on("/network/status", HTTP_GET, handleNetworkStatus);
  debugServer.on("/network/scan", HTTP_GET, handleNetworkScan);
  debugServer.on("/network/save", HTTP_POST, handleNetworkSave);
  debugServer.on("/network/delete", HTTP_POST, handleNetworkDelete);
  debugServer.on("/debug/leds", HTTP_POST, handleDiagnosticLedSet);
  debugServer.on("/debug/wifi", HTTP_POST, handleDebugWifiSet);
  debugServer.begin();
  debugHttpServerActive = true;
}

void handleDebugRoot() {
  debugServer.sendHeader("Cache-Control", "no-store");
  // send_P streams straight from flash; send() would copy the ~60KB dashboard into one heap
  // buffer each request, which fails once the heap fragments (BLE/WiFi/SPIFFS activity) and
  // was the cause of the dashboard going blank after a refresh.
  debugServer.send_P(200, "text/html", dashboardHtml, sizeof(dashboardHtml) - 1);
}

void handleDebugLogs() {
  debugServer.send(200, "text/plain", debugBuffer);
}

void handleDebugStatus() {
  char json[950];
  snprintf(json, sizeof(json),
           "{\"ahtPresent\":%s,\"ahtReadingValid\":%s,\"bqPresent\":%s,\"bqReadHealthy\":%s,\"lastTemperature\":%.3f,\"lastHumidity\":%.3f,\"lastRealFeel\":%.3f,\"lastAbsoluteHumidity\":%.3f,\"temperatureRate\":%.3f,\"humidityRate\":%.3f,\"realFeelRate\":%.3f,\"absoluteHumidityRate\":%.3f,\"lastBatteryV\":%.2f,\"intervalCitanjaMs\":%lu,\"i2cTimeoutMs\":%u,\"bqAdcDelayMs\":%u,\"bleConnected\":%s,\"webOtaActive\":%s,\"ledsEnabled\":%s,\"wifiEnabled\":%s,\"freeHeap\":%u,\"uptimeSeconds\":%lu,\"wifiRssi\":%ld,\"appCore\":%d}",
           ahtPresent ? "true" : "false",
           ahtReadingValid ? "true" : "false",
           bqPresent ? "true" : "false",
           bqReadHealthy ? "true" : "false",
           lastTemperature,
           lastHumidity,
           lastRealFeel,
           lastAbsoluteHumidity,
           temperatureRate,
           humidityRate,
           realFeelRate,
           absoluteHumidityRate,
           lastBatteryV,
           intervalCitanjaMs,
           i2cTimeoutMs,
           bqAdcDelayMs,
           deviceConnected ? "true" : "false",
           otaServerActive ? "true" : "false",
           diagnosticLedsEnabled ? "true" : "false",
           WiFi.getMode() != WIFI_OFF ? "true" : "false",
           ESP.getFreeHeap(),
           millis() / 1000,
           WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
           xPortGetCoreID());
  debugServer.send(200, "application/json", json);
}

void handleHistory() {
  uint32_t rangeSeconds = debugServer.hasArg("range") ?
                          (uint32_t)debugServer.arg("range").toInt() : 60;
  String json = "[";
  json.reserve(43000);
  bool first = true;

  if ((rangeSeconds > 0 && rangeSeconds <= 600) ||
      !persistentHistoryReady || persistentHistory.count == 0) {
    uint16_t start = (historyHead + HISTORY_SIZE - historyCount) % HISTORY_SIZE;
    uint32_t newestSeconds = historyCount ?
                             historyPoints[(historyHead + HISTORY_SIZE - 1) % HISTORY_SIZE].seconds : 0;
    uint16_t stride = historyCount > 600 ? (historyCount + 599) / 600 : 1;
    for (uint16_t i = 0; i < historyCount; i += stride) {
      const HistoryPoint &p = historyPoints[(start + i) % HISTORY_SIZE];
      if (rangeSeconds <= 600 && newestSeconds - p.seconds > rangeSeconds) continue;
      if (!first) json += ',';
      first = false;
      json += "{\"s\":" + String(p.seconds);
      json += ",\"t\":" + String(p.temperature, 2);
      json += ",\"h\":" + String(p.humidity, 2);
      json += ",\"r\":" + String(p.realFeel, 2);
      json += ",\"a\":" + String(p.absoluteHumidity, 2) + '}';
    }
  } else if (persistentHistoryReady && persistentHistory.count > 0) {
    uint32_t requestedPoints = rangeSeconds == 0 ? persistentHistory.count :
                   min(persistentHistory.count, rangeSeconds);
    uint32_t stride = (requestedPoints + 599) / 600;
    uint32_t oldest = (persistentHistory.head + persistentHistory.capacity - persistentHistory.count) %
                      persistentHistory.capacity;
    uint32_t skip = persistentHistory.count - requestedPoints;

    File file = SPIFFS.open(PERSISTENT_HISTORY_PATH, FILE_READ);
    if (file) {
      for (uint32_t logical = skip; logical < persistentHistory.count; logical += stride) {
        uint32_t physical = (oldest + logical) % persistentHistory.capacity;
        size_t offset = sizeof(PersistentHistoryHeader) +
                        (size_t)physical * sizeof(PersistentHistoryPoint);
        PersistentHistoryPoint p;
        if (!file.seek(offset, SeekSet) ||
            file.read((uint8_t*)&p, sizeof(p)) != sizeof(p)) continue;
        if (!first) json += ',';
        first = false;
        json += "{\"s\":" + String(p.sequence);
        json += ",\"t\":" + String(p.temperature100 / 100.0f, 2);
        json += ",\"h\":" + String(p.humidity100 / 100.0f, 2);
        json += ",\"r\":" + String(p.realFeel100 / 100.0f, 2);
        json += ",\"a\":" + String(p.absoluteHumidity100 / 100.0f, 2) + '}';
      }
      file.close();
    }
    if (historyCount > 0) {
      const HistoryPoint &latest = historyPoints[(historyHead + HISTORY_SIZE - 1) % HISTORY_SIZE];
      if (latest.seconds > persistentHistory.sequence) {
        if (!first) json += ',';
        json += "{\"s\":" + String(latest.seconds);
        json += ",\"t\":" + String(latest.temperature, 2);
        json += ",\"h\":" + String(latest.humidity, 2);
        json += ",\"r\":" + String(latest.realFeel, 2);
        json += ",\"a\":" + String(latest.absoluteHumidity, 2) + '}';
      }
    }
  }
  json += ']';
  debugServer.sendHeader("Cache-Control", "no-store");
  debugServer.send(200, "application/json", json);
}

void handleHistoryInterpolate() {
  uint32_t startEpoch = (uint32_t)strtoul(debugServer.arg("start").c_str(), nullptr, 10);
  uint32_t endEpoch = (uint32_t)strtoul(debugServer.arg("end").c_str(), nullptr, 10);
  if (startEpoch == 0 || endEpoch < startEpoch) {
    debugServer.send(400, "text/plain", "Neispravan opseg.");
    return;
  }
  uint32_t patched = 0;
  bool ok = interpolatePersistentRange(startEpoch, endEpoch, patched);
  debugServer.send(ok ? 200 : 500, "application/json",
                   "{\"ok\":" + String(ok ? "true" : "false") + ",\"patched\":" + String(patched) + "}");
}

void appendJsonString(String &json, const char *value) {
  json += '"';
  while (*value) {
    char c = *value++;
    if (c == '"' || c == '\\') {
      json += '\\';
    }
    if (c == '\n' || c == '\r') {
      json += ' ';
    } else {
      json += c;
    }
  }
  json += '"';
}

void loadSnapshots() {
  snapshotPreferences.begin("climate", false);
  size_t storedSize = snapshotPreferences.getBytesLength("snapshots");
  if (storedSize == sizeof(snapshotStore)) {
    snapshotPreferences.getBytes("snapshots", &snapshotStore, sizeof(snapshotStore));
  }
  if (snapshotStore.magic != SNAPSHOT_MAGIC || snapshotStore.count > MAX_SNAPSHOTS) {
    memset(&snapshotStore, 0, sizeof(snapshotStore));
    snapshotStore.magic = SNAPSHOT_MAGIC;
    snapshotStore.nextId = 1;
  }
}

void saveSnapshots() {
  snapshotPreferences.putBytes("snapshots", &snapshotStore, sizeof(snapshotStore));
}

void handleSnapshots() {
  String json = "[";
  json.reserve(512 + snapshotStore.count * 180);
  for (int i = snapshotStore.count - 1; i >= 0; i--) {
    Snapshot &item = snapshotStore.items[i];
    if (i != snapshotStore.count - 1) json += ',';
    json += "{\"id\":" + String(item.id) + ",\"capturedAt\":";
    appendJsonString(json, item.capturedAt);
    json += ",\"temperature\":" + String(item.temperature, 2);
    json += ",\"humidity\":" + String(item.humidity, 2);
    json += ",\"comment\":";
    appendJsonString(json, item.comment);
    json += '}';
  }
  json += ']';
  debugServer.send(200, "application/json", json);
}

void handleSnapshotCreate() {
  if (!debugServer.hasArg("t") || !debugServer.hasArg("h") ||
      debugServer.arg("capturedAt").length() < 10) {
    debugServer.send(400, "text/plain", "Nema merenja ili vremena");
    return;
  }

  float snapshotTemperature = debugServer.arg("t").toFloat();
  float snapshotHumidity = debugServer.arg("h").toFloat();
  if (isnan(snapshotTemperature) || isnan(snapshotHumidity) ||
      snapshotTemperature < -50.0f || snapshotTemperature > 100.0f ||
      snapshotHumidity < 0.0f || snapshotHumidity > 100.0f) {
    debugServer.send(400, "text/plain", "Neispravne snapshot vrednosti");
    return;
  }

  if (snapshotStore.count == MAX_SNAPSHOTS) {
    memmove(&snapshotStore.items[0], &snapshotStore.items[1], sizeof(Snapshot) * (MAX_SNAPSHOTS - 1));
    snapshotStore.count--;
  }

  Snapshot &item = snapshotStore.items[snapshotStore.count++];
  memset(&item, 0, sizeof(item));
  item.id = snapshotStore.nextId++;
  item.temperature = snapshotTemperature;
  item.humidity = snapshotHumidity;
  strlcpy(item.capturedAt, debugServer.arg("capturedAt").c_str(), sizeof(item.capturedAt));
  saveSnapshots();
  debugLogf("Snapshot %lu: T=%.2f H=%.2f", item.id, item.temperature, item.humidity);
  debugServer.send(201, "text/plain", "OK");
}

void handleSnapshotComment() {
  uint32_t id = (uint32_t)debugServer.arg("id").toInt();
  for (uint8_t i = 0; i < snapshotStore.count; i++) {
    if (snapshotStore.items[i].id == id) {
      strlcpy(snapshotStore.items[i].comment, debugServer.arg("comment").c_str(), sizeof(snapshotStore.items[i].comment));
      saveSnapshots();
      debugServer.send(200, "text/plain", "OK");
      return;
    }
  }
  debugServer.send(404, "text/plain", "Snapshot nije pronadjen");
}

void handleSnapshotDelete() {
  uint32_t id = (uint32_t)debugServer.arg("id").toInt();
  for (uint8_t i = 0; i < snapshotStore.count; i++) {
    if (snapshotStore.items[i].id == id) {
      if (i + 1 < snapshotStore.count) {
        memmove(&snapshotStore.items[i], &snapshotStore.items[i + 1],
                sizeof(Snapshot) * (snapshotStore.count - i - 1));
      }
      snapshotStore.count--;
      memset(&snapshotStore.items[snapshotStore.count], 0, sizeof(Snapshot));
      saveSnapshots();
      debugLogf("Snapshot %lu obrisan.", id);
      debugServer.send(200, "text/plain", "OK");
      return;
    }
  }
  debugServer.send(404, "text/plain", "Snapshot nije pronadjen");
}

void handleNetworkStatus() {
  bool connected = WiFi.status() == WL_CONNECTED;
  String json = "{\"connected\":";
  json += connected ? "true" : "false";
  json += ",\"apActive\":";
  json += wifiApFallbackActive ? "true" : "false";
  json += ",\"ssid\":";
  appendJsonString(json, connected ? WiFi.SSID().c_str() : "");
  json += ",\"ip\":";
  String ip = connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  appendJsonString(json, ip.c_str());
  json += ",\"apSsid\":";
  appendJsonString(json, wifiApFallbackActive ? OTA_AP_SSID : "");
  json += ",\"known\":[";
  for (uint8_t i = 0; i < wifiStore.count; i++) {
    if (i) json += ',';
    appendJsonString(json, wifiStore.networks[i].ssid);
  }
  json += "]}";
  debugServer.sendHeader("Cache-Control", "no-store");
  debugServer.send(200, "application/json", json);
}

void handleNetworkScan() {
  int found = WiFi.scanNetworks();
  String json = "[";
  json.reserve(128 + (found > 0 ? found * 72 : 0));
  bool first = true;
  for (int i = 0; i < found; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    if (!first) json += ',';
    first = false;
    json += "{\"ssid\":";
    appendJsonString(json, ssid.c_str());
    json += ",\"rssi\":" + String(WiFi.RSSI(i));
    json += ",\"secure\":";
    json += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true";
    json += '}';
  }
  WiFi.scanDelete();
  json += ']';
  debugServer.sendHeader("Cache-Control", "no-store");
  debugServer.send(200, "application/json", json);
}

void handleNetworkSave() {
  String ssid = debugServer.arg("ssid");
  String password = debugServer.arg("password");
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
    debugServer.send(400, "text/plain", "Neispravan SSID ili lozinka");
    return;
  }

  int index = -1;
  for (uint8_t i = 0; i < wifiStore.count; i++) {
    if (ssid == wifiStore.networks[i].ssid) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    if (wifiStore.count >= MAX_KNOWN_NETWORKS) {
      debugServer.send(409, "text/plain", "Obrisi jednu poznatu mrezu (maksimum je 5)");
      return;
    }
    index = wifiStore.count++;
    memset(&wifiStore.networks[index], 0, sizeof(KnownNetwork));
  }

  strlcpy(wifiStore.networks[index].ssid, ssid.c_str(), sizeof(wifiStore.networks[index].ssid));
  if (password.length() > 0 || wifiStore.networks[index].password[0] == '\0') {
    strlcpy(wifiStore.networks[index].password, password.c_str(), sizeof(wifiStore.networks[index].password));
  }
  saveKnownNetworks();
  debugLogf("WiFi: mreza '%s' je sacuvana.", ssid.c_str());
  networkReconfigurePending = true;
  networkReconfigureAtMs = millis() + 1500;
  debugServer.send(200, "text/plain", "Mreza je sacuvana; stanica pokusava povezivanje");
}

void handleNetworkDelete() {
  String ssid = debugServer.arg("ssid");
  for (uint8_t i = 0; i < wifiStore.count; i++) {
    if (ssid == wifiStore.networks[i].ssid) {
      if (i + 1 < wifiStore.count) {
        memmove(&wifiStore.networks[i], &wifiStore.networks[i + 1],
                sizeof(KnownNetwork) * (wifiStore.count - i - 1));
      }
      wifiStore.count--;
      memset(&wifiStore.networks[wifiStore.count], 0, sizeof(KnownNetwork));
      saveKnownNetworks();
      networkReconfigurePending = true;
      networkReconfigureAtMs = millis() + 1500;
      debugLogf("WiFi: mreza '%s' je obrisana.", ssid.c_str());
      debugServer.send(200, "text/plain", "Mreza je obrisana");
      return;
    }
  }
  debugServer.send(404, "text/plain", "Mreza nije pronadjena");
}

void handleDiagnosticLedSet() {
  if (!debugServer.hasArg("enabled")) {
    debugServer.send(400, "text/plain", "Nedostaje enabled");
    return;
  }
  diagnosticLedsEnabled = debugServer.arg("enabled") == "1";
  wifiPreferences.putBool("diag-leds", diagnosticLedsEnabled);
  if (!diagnosticLedsEnabled) {
    setDiagnosticLedsOff();
  } else {
    diagnosticLedPhaseMs = millis() - DIAGNOSTIC_LED_PERIOD_MS;
  }
  debugLogf("RGB dijagnostika: %s.", diagnosticLedsEnabled ? "ukljucena" : "iskljucena");
  debugServer.send(200, "text/plain", "OK");
}

// Rucni prekidac: gasi/pali samo WiFi radio, debug/OTA server ostaje uvek pokrenut i cim WiFi
// bude ponovo ukljucen postaje odmah dostupan bez ponovnog pokretanja.
void setWifiEnabled(bool enabled) {
  if (enabled) {
    if (WiFi.getMode() == WIFI_OFF) {
      networkReconfigurePending = true;
      networkReconfigureAtMs = millis() + 200;
      debugLogf("WiFi: rucno ukljucen preko prekidaca.");
    }
  } else {
    if (WiFi.getMode() != WIFI_OFF) {
      WiFi.mode(WIFI_OFF);
      wifiApFallbackActive = false;
      debugLogf("WiFi: rucno iskljucen preko prekidaca.");
    }
  }
}

void handleDebugWifiSet() {
  if (!debugServer.hasArg("enabled")) {
    debugServer.send(400, "text/plain", "Nedostaje enabled");
    return;
  }
  setWifiEnabled(debugServer.arg("enabled") == "1");
  debugServer.send(200, "text/plain", "OK");
}

void handleDebugSet() {
  if (debugServer.hasArg("interval")) {
    unsigned long v = (unsigned long)debugServer.arg("interval").toInt();
    if (v >= 300 && v <= 60000) intervalCitanjaMs = v;
  }
  if (debugServer.hasArg("i2c")) {
    int v = debugServer.arg("i2c").toInt();
    if (v >= 10 && v <= 500) i2cTimeoutMs = (uint16_t)v;
  }
  if (debugServer.hasArg("bq")) {
    int v = debugServer.arg("bq").toInt();
    if (v >= 20 && v <= 1000) bqAdcDelayMs = (uint16_t)v;
  }

  Wire.setTimeOut(i2cTimeoutMs);
  Wire1.setTimeOut(i2cTimeoutMs);

  if (debugServer.hasArg("aht_reinit")) {
    initAHT();
  }

  debugLogf("Param update: interval=%lu i2c=%u bq=%u", intervalCitanjaMs, i2cTimeoutMs, bqAdcDelayMs);
  debugServer.send(200, "text/plain", "OK");
}

void setup() {
  delay(300);
  debugBuffer.reserve(DEBUG_BUFFER_MAX + 256);

  debugLogf("BOOT: VoiceToysWS pokrenut; UART je iskljucen.");

  setCpuFrequencyMhz(80);
  WiFi.mode(WIFI_OFF);
  debugLogf("SISTEM: CPU=80 MHz, WiFi inicijalno iskljucen.");

  debugLogf("BLE: inicijalizacija uredjaja i servisa...");
  NimBLEDevice::init("VoiceToysWS");
  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                    );
  pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());

  pDebugCharacteristic = pService->createCharacteristic(
                      DEBUG_CHARACTERISTIC_UUID,
                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
                    );

  pHistoryCharacteristic = pService->createCharacteristic(
                      HISTORY_CHARACTERISTIC_UUID,
                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                    );
  pHistoryCharacteristic->setCallbacks(new MyHistoryCallbacks());
  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  debugLogf("BLE: aktivan, naziv=VoiceToysWS, OTA komanda='%c'.", OTA_TRIGGER_CHAR);

  debugLogf("OTA: pokretanje lokalnog WiFi/WebOTA interfejsa...");
  loadKnownNetworks();
  diagnosticLedsEnabled = wifiPreferences.getBool("diag-leds", true);
  initDiagnosticLeds();
  startDiagnosticLedTask();
  startWebOtaServer();
  loadSnapshots();
  debugLogf("Snapshot memorija: %u/%u sacuvano.", snapshotStore.count, MAX_SNAPSHOTS);
  persistentHistoryReady = initPersistentHistory();

#if ENABLE_BQ
  debugLogf("BQ: pokretanje I2C magistrale (SDA=%d, SCL=%d)...", BQ_SDA, BQ_SCL);
  Wire.begin(BQ_SDA, BQ_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(i2cTimeoutMs);
  bqPresent = probeI2C(Wire, BQ25895_ADDRESS);
  debugLogf("BQ25895 0x%02X: %s (SDA=%d SCL=%d)", BQ25895_ADDRESS,
            bqPresent ? "OK" : "NEMA ODGOVORA", BQ_SDA, BQ_SCL);

  if (bqPresent) {
    bqReadHealthy = true;
    initBQ25895();
    dumpBQRegisters();
    debugLogf("BQ read status: %s", bqReadHealthy ? "OK" : "FAIL");
    sampleBattery(true);
  }
#else
  bqPresent = false;
  debugLogf("BQ: privremeno iskljucen u programu.");
#endif

  debugLogf("AHT: pokretanje I2C magistrale (SDA=%d, SCL=%d)...", AHT_SDA_PIN, AHT_SCL_PIN);
  initAHT();
  if (ahtPresent) {
    float t = 0.0f;
    float h = 0.0f;
    if (readAHT(t, h)) {
      updateClimateMetrics(t, h);
      ahtReadingValid = true;
      debugLogf("Test AHT: T=%.2f H=%.2f RF=%.2f AH=%.2f", t, h, lastRealFeel, lastAbsoluteHumidity);
    }
  }

  debugLogf("READY: inicijalizacija zavrsena; telemetrija na svakih %lu ms.", intervalCitanjaMs);
}


struct __attribute__((packed)) BleHistoryPoint {
  uint32_t s;
  int16_t t;
  int16_t h;
};

// 2 points/notify (16 bytes) stays under the default 20-byte usable ATT MTU even before MTU negotiation completes.
const uint8_t BLE_HISTORY_CHUNK_POINTS = 2;

void loop() {
  if (historyStreamActive && deviceConnected && historyStreamHeaderPending) {
    // s=0xFFFFFFFE marks a header packet: t carries the expected point count (fits well under 32767).
    BleHistoryPoint header = {0xFFFFFFFE, (int16_t)historyStreamExpectedPoints, 0};
    pHistoryCharacteristic->setValue((uint8_t*)&header, sizeof(BleHistoryPoint));
    pHistoryCharacteristic->notify();
    historyStreamHeaderPending = false;
  } else if (historyStreamActive && deviceConnected) {
    BleHistoryPoint chunk[BLE_HISTORY_CHUNK_POINTS];
    uint8_t chunkSize = 0;
    
    if (!historyStreamPersistent) {
      uint16_t start = (historyHead + HISTORY_SIZE - historyCount) % HISTORY_SIZE;
      while (chunkSize < BLE_HISTORY_CHUNK_POINTS && historyStreamIndex < historyStreamCount) {
        const HistoryPoint &p = historyPoints[(start + historyStreamIndex) % HISTORY_SIZE];
        chunk[chunkSize].s = p.seconds;
        chunk[chunkSize].t = (int16_t)roundf(p.temperature * 100.0f);
        chunk[chunkSize].h = (int16_t)roundf(p.humidity * 100.0f);
        chunkSize++;
        historyStreamIndex += historyStreamStride;
      }
    } else {
      // Fajl se otvara jednom kad stream krene (vidi onWrite 'G') i ostaje otvoren do kraja/prekida.
      if (historyStreamFile) {
        while (chunkSize < BLE_HISTORY_CHUNK_POINTS && historyStreamLogical < historyStreamCount) {
          uint32_t physical = (historyStreamOldest + historyStreamLogical) % persistentHistory.capacity;
          size_t offset = sizeof(PersistentHistoryHeader) + (size_t)physical * sizeof(PersistentHistoryPoint);
          PersistentHistoryPoint p;
          if (historyStreamFile.seek(offset, SeekSet) && historyStreamFile.read((uint8_t*)&p, sizeof(p)) == sizeof(p)) {
            chunk[chunkSize].s = p.sequence;
            chunk[chunkSize].t = p.temperature100;
            chunk[chunkSize].h = p.humidity100;
            chunkSize++;
          }
          // Koristiti stride=1 (pun detalj) za novi region; stride za stari region.
          uint32_t step = (historyStreamLogical < historyStreamRecentStart) ? historyStreamStride : 1;
          historyStreamLogical += step;
        }
      } else {
        historyStreamActive = false; // abort if spiffs fail
      }
    }
    
    if (chunkSize > 0) {
      pHistoryCharacteristic->setValue((uint8_t*)chunk, chunkSize * sizeof(BleHistoryPoint));
      pHistoryCharacteristic->notify();
      // Daj BLE steku vremena da isprazni notifikaciju pre sledece; salju se prebrzo bez ove pauze
      // (brze od pregovorenog konekcionog intervala od 30-50ms) sto vremenom prepuni NimBLE red i obara uredjaj.
      delay(30);
    } else {
      historyStreamActive = false;
      if (historyStreamFile) historyStreamFile.close();
      // Send EOF packet
      BleHistoryPoint eof = {0xFFFFFFFF, 0, 0};
      pHistoryCharacteristic->setValue((uint8_t*)&eof, sizeof(BleHistoryPoint));
      pHistoryCharacteristic->notify();
      debugLogf("BLE History Stream zavrsen.");
    }
  }

  if (otaStartRequested) {
    otaStartRequested = false;
    startWebOtaServer();
  }

  if (otaServerActive) {
    webota.handle();
  }
  if (debugHttpServerActive) {
    debugServer.handleClient();
  }
  updateClockState();
  // Diagnostic LEDs now run on their own dedicated core/task; see startDiagnosticLedTask().

  if (networkReconfigurePending && (long)(millis() - networkReconfigureAtMs) >= 0) {
    networkReconfigurePending = false;
    configureNetwork();
    startNetworkDiscovery();
  } else if (wifiStore.count > 0 && WiFi.status() != WL_CONNECTED &&
             millis() - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    configureNetwork();
    startNetworkDiscovery();
  }

  if (millis() - zadnjeVremeCitanja >= intervalCitanjaMs) {
    zadnjeVremeCitanja = millis();

    float temperature = 0.0f;
    float humidity = 0.0f;
    bool ahtOk = readAHT(temperature, humidity);

    if (!ahtOk) {
      ahtReadingValid = false;
      if (millis() - lastAhtProbeMs >= 5000) {
        lastAhtProbeMs = millis();
        initAHT();
        debugLogf("AHT citanje neuspesno; pokusavam ponovnu inicijalizaciju.");
      }
    } else {
      ahtReadingValid = true;
    }

    sampleBattery();

    if (ahtOk) {
      updateClimateMetrics(temperature, humidity);
    }

    char asciiBuffer[80];
    if (ahtOk) {
      snprintf(asciiBuffer, sizeof(asciiBuffer), "T:%.2f,H:%.2f,V:%.2fV,S:%lu", temperature, humidity, lastBatteryV, (unsigned long)currentSampleTimestamp());
    } else {
      snprintf(asciiBuffer, sizeof(asciiBuffer), "T:NA,H:NA,V:%.2fV,S:%lu", lastBatteryV, (unsigned long)currentSampleTimestamp());
    }

    if (millis() - lastStatusLogMs >= 30000) {
      lastStatusLogMs = millis();
      debugLogf("Status: %s", asciiBuffer);
    }
    pCharacteristic->setValue((uint8_t*)asciiBuffer, strlen(asciiBuffer));
    if (deviceConnected) {
      pCharacteristic->notify();
    }
  }

  delay(10);
}
