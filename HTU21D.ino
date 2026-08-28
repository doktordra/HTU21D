#include <Wire.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebOTA.h>
#include <WebServer.h>
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

NimBLECharacteristic *pCharacteristic = nullptr;
NimBLECharacteristic *pDebugCharacteristic = nullptr;

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
String dashboardHtml;
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
// Flash se ne pise svake sekunde: 60 tacaka se upisuje odjednom na minut.
const uint32_t PERSISTENT_HISTORY_MAGIC = 0x434C494D;
const uint16_t PERSISTENT_HISTORY_VERSION = 4;
const uint32_t PERSISTENT_HISTORY_CAPACITY = 24UL * 60UL * 60UL;
const uint16_t PERSISTENT_BATCH_SIZE = 60;
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

void writeBQRegister(uint8_t reg, uint8_t value);
bool readBQRegisterWithMode(uint8_t reg, uint8_t &value, bool useRepeatedStart);
uint8_t readBQRegister(uint8_t reg);
void initBQ25895();
float getBatteryVoltage();
void dumpBQRegisters();
void sampleBattery(bool force = false);
void initDiagnosticLeds();
void updateDiagnosticLeds();
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

class MyServerCallbacks: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
    deviceConnected = true;
    debugLogf("Klijent spojen.");
    pServer->updateConnParams(desc->conn_handle, 24, 40, 0, 200);
  }

  void onDisconnect(NimBLEServer* pServer) override {
    deviceConnected = false;
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
  debugServer.begin();
  debugHttpServerActive = true;
}

void handleDebugRoot() {
  String &html = dashboardHtml;
  if (html.length() == 0) {
  html.reserve(27000);
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>VoiceToys Weather Station</title><style>:root{--ink:#17211c;--paper:#f4f1e8;--accent:#087e6a;--line:#c9c7ba;--up:#14834b;--down:#c43b32;--flat:#d2a400}";
  html += "*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font-family:Georgia,serif}";
  html += "main{width:min(900px,calc(100% - 24px));margin:24px auto}header{display:flex;justify-content:space-between;align-items:end;border-bottom:2px solid var(--ink);padding-bottom:12px;gap:10px}";
  html += "h1{font-size:24px;margin:0}.tabs,.actions{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.tabs{margin:14px 0}.tab{background:#d9d8ce;color:var(--ink)}.tab.active{background:var(--accent);color:#fff}.view{display:none}.view.active{display:block}";
  html += ".live{display:grid;gap:12px;margin:14px 0 22px}.metric{display:grid;grid-template-columns:minmax(190px,220px) 164px minmax(0,1fr);align-items:center;column-gap:8px;position:relative;border:1px solid var(--line);padding:16px;background:#fff;min-height:148px}.reading{align-self:center}.metricName{font-size:17px;font-weight:700;color:#445149;margin-bottom:4px}.value{font:700 clamp(34px,6vw,50px) Georgia,serif}.unit{font-size:17px;color:#59635d}";
  html += ".trendBox{display:grid;grid-template-columns:78px 58px;align-items:center;justify-content:start;gap:6px;min-width:164px;padding-right:14px}.trendVisual{display:grid;grid-template-rows:82px auto;justify-items:center;align-items:center}.rangeStack{height:112px;display:flex;flex-direction:column;justify-content:space-between;align-items:flex-start}.rangeLabel{font:700 20px Arial,sans-serif;color:#35443c;white-space:nowrap}.rangeDelta{font-size:16px;color:#59665f}.rangeDelta.up{color:var(--up)}.rangeDelta.down{color:var(--down)}.metric .rate{font:700 14px Arial,sans-serif;color:#45524a;white-space:nowrap;text-align:center;margin-top:3px}.trend{position:relative;width:34px;height:78px;color:var(--flat);transform:rotate(0deg);transition:transform .45s cubic-bezier(.2,.8,.2,1),color .35s ease}.trend .shaft{position:absolute;left:15px;bottom:9px;width:4px;height:var(--len,0px);max-height:56px;background:currentColor;border-radius:4px;transition:height .55s cubic-bezier(.2,.8,.2,1)}.trend .shaft:before{content:'';position:absolute;left:-5px;top:-3px;border-left:7px solid transparent;border-right:7px solid transparent;border-bottom:10px solid currentColor;transform:translateY(-7px)}.trend.down{color:var(--down);transform:rotate(180deg)}.trend.up{color:var(--up)}.trend.flat{color:var(--flat);transform:rotate(90deg)}.trend.flat .shaft{height:25px!important}.trend.flat .shaft:before{display:block}.chart{min-width:0}.chart canvas{display:block;width:100%;height:112px;touch-action:none}";
  html += "button{border:0;background:var(--accent);color:#fff;padding:11px 16px;font-weight:700;cursor:pointer}button:disabled{opacity:.5}.update{background:#263d70}.actions{margin:12px 0 22px}.note{font-size:13px;color:#59635d}";
  html += ".timeline{background:#fff;border:1px solid var(--line);padding:14px 18px;margin:10px 0 14px}.timelineTrack{position:relative;height:28px;margin:2px 5px}.timelineRail,.timelineFill{position:absolute;left:0;right:0;top:12px;height:5px;border-radius:5px;background:#d4d5cf}.timelineFill{right:auto;background:var(--accent)}.timeline input[type=range]{position:absolute;left:0;top:0;width:100%;height:28px;margin:0;padding:0;border:0;background:transparent;pointer-events:none;appearance:none}.timeline input[type=range]::-webkit-slider-thumb{appearance:none;width:22px;height:22px;border-radius:50%;background:#fff;border:4px solid var(--accent);box-shadow:0 1px 4px #0005;pointer-events:auto;cursor:grab}.timeline input[type=range]::-moz-range-thumb{width:15px;height:15px;border-radius:50%;background:#fff;border:4px solid var(--accent);box-shadow:0 1px 4px #0005;pointer-events:auto;cursor:grab}.timelineLabels{display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:10px;font:700 13px Arial,sans-serif;color:#46534c}.timelineLabels span:nth-child(2){text-align:center}.timelineLabels span:last-child{text-align:right}.snapshotReading{min-height:16px;margin-top:5px;color:#8a6320;font:12px Arial,sans-serif}.save{background:#9b6516}.hidden{display:none!important}.deleteHint{color:#8e4038}.snapshotRow{touch-action:manipulation}";
  html += "table{width:100%;border-collapse:collapse;background:#fff}th,td{text-align:left;border-bottom:1px solid var(--line);padding:10px;vertical-align:top}th{font-size:12px;text-transform:uppercase}";
  html += "input,select{width:100%;min-width:140px;padding:8px;border:1px solid var(--line);font:14px Georgia,serif}.networkCard{background:#fff;border:1px solid var(--line);padding:16px;margin:12px 0}.networkCard h2{font-size:18px;margin:0 0 10px}.networkForm{display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:end}.networkList{display:grid;gap:7px;margin-top:10px}.networkItem{display:flex;justify-content:space-between;align-items:center;gap:8px;border-bottom:1px solid var(--line);padding:7px 0}.danger{background:#9d332c}.address{font-family:Arial,sans-serif;font-weight:700;word-break:break-all}.debugGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}.debugValue{font:700 20px Arial,sans-serif}.ok{color:var(--up)}.bad{color:var(--down)}.switchLine{display:flex;align-items:center;gap:10px}.switchLine input{width:auto;min-width:0}#debugLogs{display:block;white-space:pre-wrap;overflow-wrap:anywhere;max-height:320px;overflow:auto;background:#18201c;color:#dce8df;padding:12px;font:11px monospace}#message,#networkMessage,#debugMessage{min-height:20px}";
  html += "@media(max-width:620px){main{margin:12px auto}.metric{grid-template-columns:minmax(0,1fr) 164px;padding:14px 12px;column-gap:4px;min-height:0}.metricName{font-size:16px}.value{font-size:38px}.unit{font-size:15px}.trendBox{grid-column:2;grid-row:1}.chart{grid-column:1/-1}.chart canvas{height:104px}.timeline{padding:12px 10px}.timelineLabels{font-size:11px;gap:5px}header{align-items:start;flex-direction:column}.networkForm{grid-template-columns:1fr}.networkForm button{width:100%}table,thead,tbody,tr,th,td{display:block}thead{display:none}tr{border-bottom:2px solid var(--ink)}td{border:0;padding:6px 10px}}</style></head><body><main>";
  html += "<header><h1>VoiceToys Weather Station</h1><span id='sensorState' class='note'>Povezivanje...</span></header>";
  html += "<nav class='tabs'><button id='liveTab' class='tab active' onclick=\"showTab('live')\">Uzivo</button><button id='snapTab' class='tab' onclick=\"showTab('snap')\">Snapshotovi</button><button id='networkTab' class='tab' onclick=\"showTab('network')\">Network</button><button id='debugTab' class='tab' onclick=\"showTab('debug')\">Debug</button></nav>";
  html += "<section id='liveView' class='view active'><div class='timeline'><div class='timelineTrack'><div class='timelineRail'></div><div id='timelineFill' class='timelineFill'></div><input id='rangeStart' type='range' min='0' max='1' value='0' oninput='applyTimeRange()'><input id='rangeEnd' type='range' min='0' max='1' value='1' oninput='applyTimeRange()'></div><div class='timelineLabels'><span id='recordedFrom'>--:--:--</span><span id='recordedDate'>--/--/----</span><span id='recordedTo'>--:--:--</span></div></div><section class='live'>";
  html += "<div class='metric'><div class='reading'><div class='metricName'>Temperatura</div><span id='temperature' class='value'>--</span><span class='unit'> &deg;C</span><div id='temperatureSnapshot' class='snapshotReading'></div></div><div class='trendBox'><div class='trendVisual'><div id='temperatureTrend' class='trend flat'><i class='shaft'></i></div><div id='temperatureRate' class='rate'>--</div></div><div class='rangeStack'><span id='temperatureMax' class='rangeLabel'>--</span><span id='temperatureDelta' class='rangeLabel rangeDelta'>--</span><span id='temperatureMin' class='rangeLabel'>--</span></div></div><div class='chart'><canvas id='temperatureChart'></canvas></div></div>";
  html += "<div class='metric'><div class='reading'><div class='metricName'>Relativna vlaznost</div><span id='humidity' class='value'>--</span><span class='unit'> %</span><div id='humiditySnapshot' class='snapshotReading'></div></div><div class='trendBox'><div class='trendVisual'><div id='humidityTrend' class='trend flat'><i class='shaft'></i></div><div id='humidityRate' class='rate'>--</div></div><div class='rangeStack'><span id='humidityMax' class='rangeLabel'>--</span><span id='humidityDelta' class='rangeLabel rangeDelta'>--</span><span id='humidityMin' class='rangeLabel'>--</span></div></div><div class='chart'><canvas id='humidityChart'></canvas></div></div>";
  html += "<div class='metric'><div class='reading'><div class='metricName'>RealFeel (procena)</div><span id='realFeel' class='value'>--</span><span class='unit'> &deg;C</span><div id='realFeelSnapshot' class='snapshotReading'></div></div><div class='trendBox'><div class='trendVisual'><div id='realFeelTrend' class='trend flat'><i class='shaft'></i></div><div id='realFeelRate' class='rate'>--</div></div><div class='rangeStack'><span id='realFeelMax' class='rangeLabel'>--</span><span id='realFeelDelta' class='rangeLabel rangeDelta'>--</span><span id='realFeelMin' class='rangeLabel'>--</span></div></div><div class='chart'><canvas id='realFeelChart'></canvas></div></div>";
  html += "<div class='metric'><div class='reading'><div class='metricName'>Apsolutna vlaznost</div><span id='absoluteHumidity' class='value'>--</span><span class='unit'> g/m&sup3;</span><div id='absoluteHumiditySnapshot' class='snapshotReading'></div></div><div class='trendBox'><div class='trendVisual'><div id='absoluteHumidityTrend' class='trend flat'><i class='shaft'></i></div><div id='absoluteHumidityRate' class='rate'>--</div></div><div class='rangeStack'><span id='absoluteHumidityMax' class='rangeLabel'>--</span><span id='absoluteHumidityDelta' class='rangeLabel rangeDelta'>--</span><span id='absoluteHumidityMin' class='rangeLabel'>--</span></div></div><div class='chart'><canvas id='absoluteHumidityChart'></canvas></div></div>";
  html += "</section>";
  html += "<div class='actions'><button id='snapshotButton' onclick='captureSnapshot()'>Napravi snapshot</button><button id='saveSnapshotButton' class='save hidden' onclick='savePendingSnapshot()'>Sacuvaj snapshot</button><span id='message' class='note'></span></div></section>";
  html += "<section id='snapView' class='view'><p class='note'>Snapshotovi su trajno sacuvani u ESP32 memoriji (NVS), najvise 24. <span class='deleteHint'>Drzi red oko 1 sekunde da ga obrises.</span></p><table><thead><tr><th>Vreme</th><th>Temperatura</th><th>Rel. vlaznost</th><th>RealFeel</th><th>Aps. vlaznost</th><th>Komentar</th></tr></thead><tbody id='snapshots'></tbody></table></section>";
  html += "<section id='networkView' class='view'><div class='networkCard'><h2>Status mreze</h2><div id='networkState'>Ucitavanje...</div><p class='note'>Na kucnoj mrezi otvori:</p><div class='address'>http://ws.local:8081/</div></div><div class='networkCard'><h2>Dodaj ili promeni mrezu</h2><p class='note'>Izaberi skeniranu mrezu ili upisi SSID. Lozinka se cuva samo u ESP32 NVS memoriji.</p><div class='networkForm'><label>Wi-Fi mreza<input id='networkSsid' list='availableNetworks' maxlength='32' placeholder='SSID'><datalist id='availableNetworks'></datalist></label><label>Lozinka<input id='networkPassword' type='password' maxlength='64' autocomplete='new-password' placeholder='Prazno za otvorenu mrezu'></label><button onclick='saveNetwork()'>Sacuvaj</button></div><div class='actions'><button onclick='scanNetworks()'>Pretrazi mreze</button><span id='networkMessage' class='note'></span></div></div><div class='networkCard'><h2>Zapamcene mreze</h2><div id='knownNetworks' class='networkList'></div></div><div class='networkCard'><h2>Firmware</h2><p class='note'>Update radi i preko kucne mreze i preko fallback AP mreze.</p><button class='update' onclick='openUpdate()'>Update firmware</button></div></section>";
  html += "<section id='debugView' class='view'><div class='debugGrid'><div class='networkCard'><h2>AHT senzor</h2><div id='debugSensor' class='debugValue'>--</div><div class='note'>I2C SDA 1 / SCL 3</div></div><div class='networkCard'><h2>Baterija / BQ25895</h2><div id='debugBattery' class='debugValue'>--</div><div id='debugBq' class='note'>--</div></div><div class='networkCard'><h2>BLE</h2><div id='debugBle' class='debugValue'>--</div></div><div class='networkCard'><h2>WebOTA</h2><div id='debugOta' class='debugValue'>--</div></div><div class='networkCard'><h2>Wi-Fi signal</h2><div id='debugWifi' class='debugValue'>--</div></div><div class='networkCard'><h2>Sistem</h2><div id='debugSystem' class='debugValue'>--</div></div></div><div class='networkCard'><h2>RGB dijagnostika</h2><label class='switchLine'><input id='diagnosticLedToggle' type='checkbox' onchange='setDiagnosticLeds(this.checked)'>Kratak impuls na svake 3 sekunde</label><p class='note'>LED 1: baterija zeleno / zuto / crveno. LED 2: AHT senzor zeleno / crveno. Osvetljenje je ograniceno radi stednje baterije.</p><span id='debugMessage' class='note'></span></div><div class='networkCard'><h2>Debug log</h2><button onclick='loadDebug()'>Osvezi</button><pre id='debugLogs'>--</pre></div></section>";
  html += "<script>";
  html += "let live=null;const $=id=>document.getElementById(id);function setTrend(name,rate,scale,unit){const el=$(name+'Trend'),out=$(name+'Rate'),a=Math.abs(rate),flat=a<.005;el.className='trend '+(flat?'flat':rate>0?'up':'down');el.style.setProperty('--len',Math.min(56,12+a*scale)+'px');out.textContent=flat?'stabilno':(rate>0?'+':'')+rate.toFixed(a<.1?2:1)+' '+unit+'/min';}";
  html += "async function refresh(){try{live=await fetch('/status',{cache:'no-store'}).then(r=>r.json());const ok=live.ahtReadingValid;temperature.textContent=ok?live.lastTemperature.toFixed(2):'--';humidity.textContent=ok?live.lastHumidity.toFixed(2):'--';realFeel.textContent=ok?live.lastRealFeel.toFixed(2):'--';absoluteHumidity.textContent=ok?live.lastAbsoluteHumidity.toFixed(2):'--';if(ok){setTrend('temperature',live.temperatureRate,18,'C');setTrend('humidity',live.humidityRate,5,'%');setTrend('realFeel',live.realFeelRate,18,'C');setTrend('absoluteHumidity',live.absoluteHumidityRate,14,'g/m3')}sensorState.textContent=ok?'Senzor je aktivan · osvezavanje 0.5 s':(live.ahtPresent?'Ceka se prvo merenje':'Senzor nije pronadjen');}catch(e){sensorState.textContent='Nema veze sa uredjajem';}}";
  html += "function showTab(t){liveView.classList.toggle('active',t==='live');snapView.classList.toggle('active',t==='snap');networkView.classList.toggle('active',t==='network');debugView.classList.toggle('active',t==='debug');liveTab.classList.toggle('active',t==='live');snapTab.classList.toggle('active',t==='snap');networkTab.classList.toggle('active',t==='network');debugTab.classList.toggle('active',t==='debug');if(t==='snap')loadSnapshots();if(t==='network')loadNetwork();if(t==='debug')loadDebug()}function openUpdate(){location.href='http://'+location.hostname+':8080/webota'}";
  html += "let allChartData=[],chartData=[],cursorIndex=-1,rangeReady=false,rangeWindowSize=0,liveRangeFollow=true;const chartDefs=[['temperature','t','#c76432',.6,'C'],['humidity','h','#2878b8',1.5,'%'],['realFeel','r','#8b4aa5',.6,'C'],['absoluteHumidity','a','#087e6a',.6,'g/m3']];function clamp(v,min,max){return Math.min(max,Math.max(min,v))}function formatMoment(s,full=false){if(s>1700000000){const d=new Date(s*1000);return full?d.toLocaleString():d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'})}const h=Math.floor(s/3600),m=Math.floor(s%3600/60),q=s%60;return'T+'+[h,m,q].map(n=>String(n).padStart(2,'0')).join(':')}function formatDate(s){return s>1700000000?new Date(s*1000).toLocaleDateString():'od ukljucenja'}function nearestIndex(data,t){if(!data.length)return 0;let best=0,dist=Infinity;for(let i=0;i<data.length;i++){const d=Math.abs(data[i].s-t);if(d<dist){dist=d;best=i}}return best}";
  html += "function drawChart(name,data,key,color,minSpan){const c=$(name+'Chart'),d=devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;c.width=w*d;c.height=h*d;const x=c.getContext('2d');x.setTransform(1,0,0,1,0,0);x.clearRect(0,0,c.width,c.height);x.scale(d,d);const v=data.map(p=>p[key]),deltaEl=$(name+'Delta');if(v.length<2){$(name+'Max').textContent=$(name+'Min').textContent=deltaEl.textContent='--';return}const rawLo=Math.min(...v),rawHi=Math.max(...v),change=v[v.length-1]-v[0],shownChange=Math.abs(change)<.05?0:change;let lo=rawLo,hi=rawHi,mid=(lo+hi)/2;if(hi-lo<minSpan){lo=mid-minSpan/2;hi=mid+minSpan/2}$(name+'Max').textContent=rawHi.toFixed(1);$(name+'Min').textContent=rawLo.toFixed(1);deltaEl.textContent=(shownChange>0?'+':'')+shownChange.toFixed(1);deltaEl.className='rangeLabel rangeDelta '+(shownChange>0?'up':shownChange<0?'down':'');const first=data[0].s,last=data[data.length-1].s,span=Math.max(1,last-first),pts=data.map((p,i)=>({x:(p.s-first)*w/span,y:h-5-(v[i]-lo)/(hi-lo)*(h-10)}));x.strokeStyle='#deddd4';x.beginPath();x.moveTo(0,h-1);x.lineTo(w,h-1);x.stroke();x.strokeStyle=color;x.lineWidth=2;x.lineJoin='round';x.lineCap='round';x.beginPath();x.moveTo(pts[0].x,pts[0].y);for(let i=1;i<pts.length-1;i++){const mx=(pts[i].x+pts[i+1].x)/2,my=(pts[i].y+pts[i+1].y)/2;x.quadraticCurveTo(pts[i].x,pts[i].y,mx,my)}x.lineTo(pts[pts.length-1].x,pts[pts.length-1].y);x.stroke();if(cursorIndex>=0&&cursorIndex<pts.length){const p=pts[cursorIndex];const metricMeta={t:{label:'T',unit:'C'},h:{label:'RH',unit:'%'},r:{label:'RF',unit:'C'},a:{label:'AH',unit:'g/m3'}};const meta=metricMeta[key]||{label:key.toUpperCase(),unit:''};const valueText=`${meta.label} ${Number(data[cursorIndex][key]).toFixed(2)} ${meta.unit}`;const labelWidth=120,labelHeight=38,labelX=Math.min(w-labelWidth-8,Math.max(8,p.x+12)),labelY=Math.max(10,p.y-labelHeight-12);x.save();x.strokeStyle='#17211c';x.lineWidth=1;x.setLineDash([4,3]);x.beginPath();x.moveTo(p.x,0);x.lineTo(p.x,h);x.stroke();x.restore();x.fillStyle='#f7f4ee';x.strokeStyle='#17211c';x.lineWidth=1;x.fillRect(labelX,labelY,labelWidth,labelHeight);x.strokeRect(labelX,labelY,labelWidth,labelHeight);x.fillStyle='#17211c';x.font='11px Arial';x.fillText(formatMoment(data[cursorIndex].s,true),labelX+8,labelY+14);x.fillText(valueText,labelX+8,labelY+28);x.fillStyle=color;x.beginPath();x.arc(p.x,p.y,4,0,Math.PI*2);x.fill();}}";
  html += "function renderCharts(){if(chartData.length&&cursorIndex>=chartData.length)cursorIndex=chartData.length-1;chartDefs.forEach(c=>drawChart(c[0],chartData,c[1],c[2],c[3]));}";
  html += "function applyTimeRange(){const n=allChartData.length;if(n<2){chartData=allChartData;renderCharts();return}const isManual=document.activeElement===rangeStart||document.activeElement===rangeEnd;if(isManual){liveRangeFollow=false;}let a=clamp(Math.round(Number(rangeStart.value)),0,n-1);let b=clamp(Math.round(Number(rangeEnd.value)),a,n-1);if(b<=a){if(document.activeElement===rangeStart){b=Math.min(n-1,a+1)}else{a=Math.max(0,b-1)}}if(liveRangeFollow && b>=n-1){const windowSize=Math.max(1,rangeWindowSize||60);a=Math.max(0,n-windowSize);b=n-1}else if(rangeWindowSize>0 && !liveRangeFollow){const maxWindow=Math.max(1,rangeWindowSize);b=Math.min(n-1,a+maxWindow);if(b<=a){b=Math.min(n-1,a+1)}}rangeStart.value=a;rangeEnd.value=b;rangeWindowSize=Math.max(1,b-a);const max=n-1,start=allChartData[a].s,end=allChartData[b].s,d1=formatDate(start),d2=formatDate(end);timelineFill.style.left=a/max*100+'%';timelineFill.style.width=(b-a)/max*100+'%';recordedFrom.textContent=formatMoment(start);recordedTo.textContent=formatMoment(end);recordedDate.textContent=d1===d2?d1:d1+' / '+d2;chartData=allChartData.slice(a,b+1);if(cursorIndex>=chartData.length)cursorIndex=chartData.length-1;renderCharts()}";
  html += "async function refreshCharts(){try{const userDragging=document.activeElement===rangeStart||document.activeElement===rangeEnd;if(userDragging){liveRangeFollow=false;}const oldWindow=rangeReady?Math.max(1,Number(rangeEnd.value)-Number(rangeStart.value)):0;const wasAtEnd=rangeReady&&Number(rangeEnd.value)>=Math.max(0,allChartData.length-1);const d=await fetch('/history?range=86400',{cache:'no-store'}).then(r=>r.json());if(!d.length)return;allChartData=d;const max=d.length-1;rangeStart.max=rangeEnd.max=max;if(!rangeReady){rangeStart.value=0;rangeEnd.value=max;rangeReady=true;liveRangeFollow=true;rangeWindowSize=max;}else if(wasAtEnd || liveRangeFollow){const windowSize=Math.max(1,oldWindow||60);rangeStart.value=Math.max(0,max-windowSize);rangeEnd.value=max;liveRangeFollow=true;rangeWindowSize=windowSize}else{rangeWindowSize=Math.max(1,rangeWindowSize||oldWindow||1);rangeStart.value=Math.min(Number(rangeStart.value),max);rangeEnd.value=Math.min(Math.max(Number(rangeEnd.value),Number(rangeStart.value)),max)}applyTimeRange()}catch(e){}}";
  html += "function setCursorFromEvent(e){if(chartData.length<2)return;const r=e.currentTarget.getBoundingClientRect(),ratio=Math.max(0,Math.min(1,(e.clientX-r.left)/r.width));cursorIndex=Math.round(ratio*(chartData.length-1));renderCharts()}function bindChartCursors(){chartDefs.forEach(c=>{const el=$(c[0]+'Chart');el.addEventListener('pointermove',setCursorFromEvent);el.addEventListener('pointerdown',setCursorFromEvent);el.addEventListener('pointerleave',e=>{if(e.pointerType==='mouse'){cursorIndex=-1;renderCharts()}})})}";
  html += "function esc(v){const d=document.createElement('div');d.textContent=v||'';return d.innerHTML}";
  html += "function escAttr(v){return esc(v).replace(/'/g,'&#39;')}";
  html += "function derived(t,h){const e=h/100*6.105*Math.exp(17.27*t/(237.7+t));const r=t+.33*e-4;const es=6.112*Math.exp(17.67*t/(t+243.5));const a=216.7*(es*h/100)/(273.15+t);return{r,a}}";
  html += "let deleteTimer=null;function startDelete(id){cancelDelete();deleteTimer=setTimeout(()=>deleteSnapshot(id),900)}function cancelDelete(){if(deleteTimer){clearTimeout(deleteTimer);deleteTimer=null}}async function deleteSnapshot(id){cancelDelete();if(!confirm('Obrisati ovaj snapshot?'))return;const p=new URLSearchParams({id});const r=await fetch('/snapshot/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});message.textContent=r.ok?'Snapshot je obrisan.':'Brisanje nije uspelo.';await loadSnapshots()}";
  html += "async function loadSnapshots(){const list=await fetch('/snapshots').then(r=>r.json());snapshots.innerHTML=list.map(s=>{const d=derived(s.temperature,s.humidity);return`<tr class='snapshotRow' onpointerdown='startDelete(${s.id})' onpointerup='cancelDelete()' onpointercancel='cancelDelete()' onpointerleave='cancelDelete()'><td>${esc(new Date(s.capturedAt).toLocaleString())}</td><td>${s.temperature.toFixed(2)} &deg;C</td><td>${s.humidity.toFixed(2)} %</td><td>${d.r.toFixed(2)} &deg;C</td><td>${d.a.toFixed(2)} g/m&sup3;</td><td><input maxlength='95' value='${escAttr(s.comment)}' onpointerdown='event.stopPropagation()' onchange='saveComment(${s.id},this.value)'></td></tr>`}).join('');}";
  html += "let pendingSnapshot=null;async function captureSnapshot(){snapshotButton.disabled=true;message.textContent='Pravim snapshot...';await refresh();if(!live||!live.ahtReadingValid){message.textContent='Nema ispravnog ocitavanja.';snapshotButton.disabled=false;return;}pendingSnapshot={capturedAt:new Date().toISOString(),temperature:live.lastTemperature,humidity:live.lastHumidity,realFeel:live.lastRealFeel,absoluteHumidity:live.lastAbsoluteHumidity};temperatureSnapshot.textContent='snapshot: '+pendingSnapshot.temperature.toFixed(2)+' C';humiditySnapshot.textContent='snapshot: '+pendingSnapshot.humidity.toFixed(2)+' %';realFeelSnapshot.textContent='snapshot: '+pendingSnapshot.realFeel.toFixed(2)+' C';absoluteHumiditySnapshot.textContent='snapshot: '+pendingSnapshot.absoluteHumidity.toFixed(2)+' g/m3';saveSnapshotButton.classList.remove('hidden');snapshotButton.disabled=false;message.textContent='Snapshot je pripremljen. Klikni Sacuvaj snapshot.';}";
  html += "async function savePendingSnapshot(){if(!pendingSnapshot)return;saveSnapshotButton.disabled=true;const s=pendingSnapshot,p=new URLSearchParams({capturedAt:s.capturedAt,t:s.temperature,h:s.humidity});const r=await fetch('/snapshot',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});message.textContent=r.ok?'Snapshot je trajno sacuvan.':'Cuvanje nije uspelo.';saveSnapshotButton.disabled=false;if(r.ok){pendingSnapshot=null;saveSnapshotButton.classList.add('hidden');temperatureSnapshot.textContent='';humiditySnapshot.textContent='';realFeelSnapshot.textContent='';absoluteHumiditySnapshot.textContent='';await loadSnapshots()}}";
  html += "async function saveComment(id,comment){const p=new URLSearchParams({id,comment});await fetch('/snapshot/comment',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});message.textContent='Komentar je sacuvan.';}";
  html += "let knownNetworkNames=[];async function loadNetwork(){try{const n=await fetch('/network/status',{cache:'no-store'}).then(r=>r.json());networkState.innerHTML=n.connected?`Povezano na <b>${esc(n.ssid)}</b><br>IP: <span class='address'>${esc(n.ip)}</span>`:`Fallback AP je aktivan: <b>${esc(n.apSsid)}</b><br>IP: <span class='address'>${esc(n.ip)}</span>`;knownNetworkNames=n.known;knownNetworks.innerHTML=n.known.length?n.known.map((s,i)=>`<div class='networkItem'><span>${esc(s)}</span><button class='danger' onclick='deleteNetworkByIndex(${i})'>Obrisi</button></div>`).join(''):`<span class='note'>Nema zapamcenih mreza.</span>`}catch(e){networkState.textContent='Status trenutno nije dostupan.'}}";
  html += "async function scanNetworks(){networkMessage.textContent='Pretrazujem...';try{const list=await fetch('/network/scan',{cache:'no-store'}).then(r=>r.json()),seen=new Set();availableNetworks.innerHTML='';list.filter(n=>!seen.has(n.ssid)&&seen.add(n.ssid)).sort((a,b)=>b.rssi-a.rssi).forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.label=n.rssi+' dBm'+(n.secure?' · zakljucana':' · otvorena');availableNetworks.appendChild(o)});networkMessage.textContent=list.length?'Izaberi mrezu ili upisi skriveni SSID.':'Nijedna mreza nije pronadjena; SSID mozes uneti rucno.'}catch(e){networkMessage.textContent='Pretraga nije uspela.'}}";
  html += "async function saveNetwork(){const ssid=networkSsid.value;if(!ssid){networkMessage.textContent='Prvo izaberi mrezu.';return}networkMessage.textContent='Cuvam mrezu...';const p=new URLSearchParams({ssid,password:networkPassword.value});try{const r=await fetch('/network/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p}),text=await r.text();networkMessage.textContent=r.ok?'Sacuvano. Telefon vrati na kucni Wi-Fi, pa otvori ws.local:8081':text;if(r.ok)networkPassword.value=''}catch(e){networkMessage.textContent='Veza je prekinuta radi povezivanja. Vrati telefon na kucni Wi-Fi i otvori ws.local:8081'}}";
  html += "function deleteNetworkByIndex(i){if(i>=0&&i<knownNetworkNames.length)deleteNetwork(knownNetworkNames[i])}async function deleteNetwork(ssid){if(!confirm('Obrisati mrezu '+ssid+'?'))return;const p=new URLSearchParams({ssid});const r=await fetch('/network/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});networkMessage.textContent=await r.text();if(r.ok)await loadNetwork()}";
  html += "function health(el,ok,text){el.className='debugValue '+(ok?'ok':'bad');el.textContent=text}async function loadDebug(){try{const [s,logs]=await Promise.all([fetch('/status',{cache:'no-store'}).then(r=>r.json()),fetch('/logs',{cache:'no-store'}).then(r=>r.text())]);health(debugSensor,s.ahtReadingValid,s.ahtReadingValid?'Radi · '+s.lastTemperature.toFixed(2)+' C / '+s.lastHumidity.toFixed(2)+' %':s.ahtPresent?'Pronadjen, bez validnog merenja':'Nije pronadjen');health(debugBattery,s.bqPresent&&s.bqReadHealthy,s.lastBatteryV>0?s.lastBatteryV.toFixed(2)+' V':'Nema merenja');debugBq.textContent=s.bqPresent?(s.bqReadHealthy?'BQ komunikacija je ispravna':'BQ greska pri citanju'):'BQ nije pronadjen';health(debugBle,true,s.bleConnected?'Klijent povezan':'Aktivan · nema klijenta');health(debugOta,s.webOtaActive,s.webOtaActive?'Aktivan · port 8080':'Nije aktivan');health(debugWifi,s.wifiRssi<0,s.wifiRssi<0?s.wifiRssi+' dBm':'Fallback AP');debugSystem.textContent=Math.round(s.freeHeap/1024)+' KB · '+Math.floor(s.uptimeSeconds/60)+' min · core '+s.appCore;diagnosticLedToggle.checked=s.ledsEnabled;debugLogs.textContent=logs||'Log je prazan.'}catch(e){debugMessage.textContent='Debug podaci nisu dostupni.'}}";
  html += "async function setDiagnosticLeds(enabled){debugMessage.textContent='Cuvam...';const p=new URLSearchParams({enabled:enabled?'1':'0'});const r=await fetch('/debug/leds',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});debugMessage.textContent=r.ok?(enabled?'RGB dijagnostika je ukljucena.':'RGB dijagnostika je iskljucena.'):'Promena nije uspela.'}";
  html += "bindChartCursors();setInterval(refresh,500);setInterval(refreshCharts,5000);refresh();refreshCharts();loadSnapshots();";
  html += "</script></main></body></html>";
  }
  debugServer.sendHeader("Cache-Control", "no-store");
  debugServer.send(200, "text/html", html);
}

void handleDebugLogs() {
  debugServer.send(200, "text/plain", debugBuffer);
}

void handleDebugStatus() {
  char json[900];
  snprintf(json, sizeof(json),
           "{\"ahtPresent\":%s,\"ahtReadingValid\":%s,\"bqPresent\":%s,\"bqReadHealthy\":%s,\"lastTemperature\":%.3f,\"lastHumidity\":%.3f,\"lastRealFeel\":%.3f,\"lastAbsoluteHumidity\":%.3f,\"temperatureRate\":%.3f,\"humidityRate\":%.3f,\"realFeelRate\":%.3f,\"absoluteHumidityRate\":%.3f,\"lastBatteryV\":%.2f,\"intervalCitanjaMs\":%lu,\"i2cTimeoutMs\":%u,\"bqAdcDelayMs\":%u,\"bleConnected\":%s,\"webOtaActive\":%s,\"ledsEnabled\":%s,\"freeHeap\":%u,\"uptimeSeconds\":%lu,\"wifiRssi\":%ld,\"appCore\":%d}",
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
  dashboardHtml.reserve(27000);
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

void loop() {
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
  updateDiagnosticLeds();

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

    char asciiBuffer[64];
    if (ahtOk) {
      snprintf(asciiBuffer, sizeof(asciiBuffer), "T:%.2f,H:%.2f,V:%.2fV", temperature, humidity, lastBatteryV);
    } else {
      snprintf(asciiBuffer, sizeof(asciiBuffer), "T:NA,H:NA,V:%.2fV", lastBatteryV);
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
