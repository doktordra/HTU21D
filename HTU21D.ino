#include <Wire.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebOTA.h>
#include <WebServer.h>
#include <Adafruit_AHTX0.h>
#include <stdarg.h>

#define AHT_SDA_PIN 15
#define AHT_SCL_PIN 2

#define BQ_SDA 33
#define BQ_SCL 13
#define BQ25895_ADDRESS 0x6A

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

unsigned long zadnjeVremeCitanja = 0;
unsigned long intervalCitanjaMs = 3000;
unsigned long lastAhtProbeMs = 0;

uint16_t i2cTimeoutMs = 50;
uint16_t bqAdcDelayMs = 120;

bool ahtPresent = false;
bool bqPresent = false;
bool bqReadHealthy = true;

String debugBuffer = "";
const size_t DEBUG_BUFFER_MAX = 4096;
WebServer debugServer(8081);

float lastTemperature = 0.0f;
float lastHumidity = 0.0f;
float lastBatteryV = 0.0f;

Adafruit_AHTX0 aht;

void debugLogf(const char* fmt, ...);
void startWebOtaServer();
void setupDebugHttpServer();
void handleDebugRoot();
void handleDebugLogs();
void handleDebugStatus();
void handleDebugSet();

bool probeI2C(TwoWire &bus, uint8_t addr);
bool initAHT();
bool readAHT(float &temperature, float &humidity);

void writeBQRegister(uint8_t reg, uint8_t value);
bool readBQRegisterWithMode(uint8_t reg, uint8_t &value, bool useRepeatedStart);
uint8_t readBQRegister(uint8_t reg);
void initBQ25895();
float getBatteryVoltage();
void dumpBQRegisters();

void debugLogf(const char* fmt, ...) {
  char buf[180];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  String line = String(buf);
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
  uint8_t reg02 = readBQRegister(0x02);
  reg02 &= ~(1 << 6);
  reg02 |= (1 << 7);
  writeBQRegister(0x02, reg02);
  delay(bqAdcDelayMs);

  uint8_t reg0E = readBQRegister(0x0E);
  debugLogf("BQ REG0E (VBAT) = 0x%02X", reg0E);
  return 2.304f + ((reg0E & 0x7F) * 0.020f);
}

void dumpBQRegisters() {
  const uint8_t regs[] = {0x00, 0x02, 0x03, 0x07, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F, 0x11, 0x14};
  for (uint8_t i = 0; i < sizeof(regs); i++) {
    debugLogf("BQ REG%02X = 0x%02X", regs[i], readBQRegister(regs[i]));
  }
}

void startWebOtaServer() {
  if (otaServerActive) {
    debugLogf("OTA server je vec aktivan.");
    return;
  }

  debugLogf("Pokrecem WiFi AP i WebOTA server...");
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(OTA_AP_SSID)) {
    debugLogf("Greska pri pokretanju softAP mreze.");
    return;
  }

  debugLogf("OTA AP IP: %s", WiFi.softAPIP().toString().c_str());

  if (!webota.init(8080, "/webota")) {
    debugLogf("Greska pri pokretanju WebOTA servera.");
    return;
  }

  otaServerActive = true;
  setupDebugHttpServer();
  debugLogf("WebOTA spreman. Otvori /webota preko AP mreze.");
  debugLogf("Debug panel: http://%s:8081/", WiFi.softAPIP().toString().c_str());
}

void setupDebugHttpServer() {
  if (debugHttpServerActive) {
    return;
  }

  debugServer.on("/", HTTP_GET, handleDebugRoot);
  debugServer.on("/logs", HTTP_GET, handleDebugLogs);
  debugServer.on("/status", HTTP_GET, handleDebugStatus);
  debugServer.on("/set", HTTP_GET, handleDebugSet);
  debugServer.begin();
  debugHttpServerActive = true;
}

void handleDebugRoot() {
  String html;
  html.reserve(2600);
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>VoiceToysWS Debug</title><style>body{font-family:monospace;background:#0b1020;color:#dce3ff;padding:16px;}";
  html += "h1{font-size:18px;margin:0 0 12px;}fieldset{border:1px solid #3a4466;margin:0 0 10px;padding:10px;}";
  html += "label{display:block;margin:6px 0;}input{width:120px;}button{padding:6px 10px;margin-top:6px;}";
  html += "pre{background:#111830;border:1px solid #2b3559;padding:10px;max-height:45vh;overflow:auto;white-space:pre-wrap;}";
  html += "small{color:#a9b5e5;}</style></head><body>";
  html += "<h1>VoiceToysWS Debug Panel</h1>";
  html += "<small>OTA upload je na /webota, debug je ovde.</small>";
  html += "<fieldset><legend>Parametri (runtime)</legend>";
  html += "<label>intervalCitanjaMs <input id='interval' type='number' min='300' max='60000'></label>";
  html += "<label>i2cTimeoutMs <input id='i2c' type='number' min='10' max='500'></label>";
  html += "<label>bqAdcDelayMs <input id='bq' type='number' min='20' max='1000'></label>";
  html += "<button onclick='apply()'>Primeni</button>";
  html += "<button onclick='reinitAht()'>Reinit AHT</button></fieldset>";
  html += "<pre id='status'>status...</pre><pre id='logs'>logs...</pre>";
  html += "<script>";
  html += "async function refresh(){const s=await fetch('/status').then(r=>r.json());";
  html += "interval.value=s.intervalCitanjaMs;i2c.value=s.i2cTimeoutMs;bq.value=s.bqAdcDelayMs;";
  html += "status.textContent=JSON.stringify(s,null,2);logs.textContent=await fetch('/logs').then(r=>r.text());}";
  html += "async function apply(){await fetch('/set?interval='+interval.value+'&i2c='+i2c.value+'&bq='+bq.value);await refresh();}";
  html += "async function reinitAht(){await fetch('/set?aht_reinit=1');await refresh();}";
  html += "setInterval(refresh,1500);refresh();";
  html += "</script></body></html>";
  debugServer.send(200, "text/html", html);
}

void handleDebugLogs() {
  debugServer.send(200, "text/plain", debugBuffer);
}

void handleDebugStatus() {
  char json[320];
  snprintf(json, sizeof(json),
           "{\"ahtPresent\":%s,\"bqPresent\":%s,\"bqReadHealthy\":%s,\"lastTemperature\":%.2f,\"lastHumidity\":%.2f,\"lastBatteryV\":%.2f,\"intervalCitanjaMs\":%lu,\"i2cTimeoutMs\":%u,\"bqAdcDelayMs\":%u}",
           ahtPresent ? "true" : "false",
           bqPresent ? "true" : "false",
           bqReadHealthy ? "true" : "false",
           lastTemperature,
           lastHumidity,
           lastBatteryV,
           intervalCitanjaMs,
           i2cTimeoutMs,
           bqAdcDelayMs);
  debugServer.send(200, "application/json", json);
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
  setCpuFrequencyMhz(80);
  WiFi.mode(WIFI_OFF);

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

  debugLogf("--- VoiceToysWS boot (80MHz) ---");
  debugLogf("OTA trigger char: '%c'", OTA_TRIGGER_CHAR);
  debugLogf("BLE aktivan i oglasava se.");

  startWebOtaServer();

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
    debugLogf("Test VBAT: %.2fV", getBatteryVoltage());
  }

  initAHT();
  if (ahtPresent) {
    float t = 0.0f;
    float h = 0.0f;
    if (readAHT(t, h)) {
      debugLogf("Test AHT: T=%.2f H=%.2f", t, h);
    }
  }

  debugLogf("Uredjaj spreman, cekam konekciju.");
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

  if (millis() - zadnjeVremeCitanja >= intervalCitanjaMs) {
    zadnjeVremeCitanja = millis();

    float temperature = 0.0f;
    float humidity = 0.0f;
    bool ahtOk = readAHT(temperature, humidity);

    if (!ahtOk) {
      if (millis() - lastAhtProbeMs >= 5000) {
        lastAhtProbeMs = millis();
        initAHT();
      }
      debugLogf("AHT citanje neuspesno.");
    }

    float vbat = bqPresent ? getBatteryVoltage() : 0.0f;

    if (ahtOk) {
      lastTemperature = temperature;
      lastHumidity = humidity;
    }
    lastBatteryV = vbat;

    char asciiBuffer[64];
    if (ahtOk) {
      snprintf(asciiBuffer, sizeof(asciiBuffer), "T:%.2f,H:%.2f,V:%.2fV", temperature, humidity, vbat);
    } else {
      snprintf(asciiBuffer, sizeof(asciiBuffer), "T:NA,H:NA,V:%.2fV", vbat);
    }

    debugLogf("Status: %s", asciiBuffer);
    pCharacteristic->setValue((uint8_t*)asciiBuffer, strlen(asciiBuffer));
    if (deviceConnected) {
      pCharacteristic->notify();
    }
  }

  delay(10);
}
