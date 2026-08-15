#include <Wire.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebOTA.h>
#include <WebServer.h>
#include <Adafruit_AHTX0.h>
#include <Preferences.h>
#include <stdarg.h>

#define AHT_SDA_PIN 22
#define AHT_SCL_PIN 19

#define BQ_SDA 33
#define BQ_SCL 13
#define BQ25895_ADDRESS 0x6A
#define ENABLE_BQ 0

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
bool ahtReadingValid = false;
bool bqPresent = false;
bool bqReadHealthy = true;

String debugBuffer = "";
const size_t DEBUG_BUFFER_MAX = 4096;
WebServer debugServer(8081);
Preferences snapshotPreferences;

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

Adafruit_AHTX0 aht;

void debugLogf(const char* fmt, ...);
void startWebOtaServer();
void setupDebugHttpServer();
void handleDebugRoot();
void handleDebugLogs();
void handleDebugStatus();
void handleDebugSet();
void handleSnapshots();
void handleSnapshotCreate();
void handleSnapshotComment();
void loadSnapshots();
void saveSnapshots();
void appendJsonString(String &json, const char *value);

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
  char message[180];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  char lineBuffer[220];
  snprintf(lineBuffer, sizeof(lineBuffer), "[%10lu ms] %s", millis(), message);
  String line = String(lineBuffer);

  Serial.println(line);

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
  debugServer.on("/snapshots", HTTP_GET, handleSnapshots);
  debugServer.on("/snapshot", HTTP_POST, handleSnapshotCreate);
  debugServer.on("/snapshot/comment", HTTP_POST, handleSnapshotComment);
  debugServer.begin();
  debugHttpServerActive = true;
}

void handleDebugRoot() {
  String html;
  html.reserve(6200);
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Klima merenje</title><style>:root{--ink:#17211c;--paper:#f4f1e8;--accent:#087e6a;--line:#c9c7ba}";
  html += "*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font-family:Georgia,serif}";
  html += "main{width:min(900px,calc(100% - 32px));margin:32px auto}header{display:flex;justify-content:space-between;align-items:end;border-bottom:2px solid var(--ink);padding-bottom:12px}";
  html += "h1{font-size:24px;margin:0;letter-spacing:0}.live{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:24px 0}";
  html += ".metric{border:1px solid var(--line);padding:24px;background:#fff}.value{font:700 48px Georgia,serif}.unit{font-size:18px;color:#59635d}";
  html += "button{border:0;background:var(--accent);color:#fff;padding:11px 16px;font-weight:700;cursor:pointer}button:disabled{opacity:.5}";
  html += ".actions{display:flex;gap:12px;align-items:center;margin-bottom:28px}.note{font-size:13px;color:#59635d}";
  html += "table{width:100%;border-collapse:collapse;background:#fff}th,td{text-align:left;border-bottom:1px solid var(--line);padding:10px;vertical-align:top}th{font-size:12px;text-transform:uppercase}";
  html += "input{width:100%;min-width:140px;padding:8px;border:1px solid var(--line);font:14px Georgia,serif}.loc{font-size:12px}#message{min-height:20px}";
  html += "@media(max-width:620px){main{margin:18px auto}.live{grid-template-columns:1fr}.value{font-size:40px}table,thead,tbody,tr,th,td{display:block}thead{display:none}tr{border-bottom:2px solid var(--ink)}td{border:0;padding:6px 10px}}</style></head><body><main>";
  html += "<header><h1>Klima merenje</h1><span id='sensorState' class='note'>Povezivanje...</span></header>";
  html += "<section class='live'><div class='metric'><div class='note'>Temperatura</div><span id='temperature' class='value'>--</span><span class='unit'> &deg;C</span></div>";
  html += "<div class='metric'><div class='note'>Relativna vlaznost</div><span id='humidity' class='value'>--</span><span class='unit'> %</span></div></section>";
  html += "<div class='actions'><button id='snapshotButton' onclick='captureSnapshot()'>Napravi snapshot</button><span id='message' class='note'></span></div>";
  html += "<table><thead><tr><th>Vreme</th><th>Temperatura</th><th>Vlaznost</th><th>Lokacija</th><th>Komentar</th></tr></thead><tbody id='snapshots'></tbody></table>";
  html += "<script>";
  html += "let live=null;async function refresh(){try{live=await fetch('/status').then(r=>r.json());temperature.textContent=live.ahtReadingValid?live.lastTemperature.toFixed(2):'--';humidity.textContent=live.ahtReadingValid?live.lastHumidity.toFixed(2):'--';sensorState.textContent=live.ahtReadingValid?'Senzor je aktivan':(live.ahtPresent?'Ceka se prvo merenje':'Senzor nije pronadjen');}catch(e){sensorState.textContent='Nema veze sa uredjajem';}}";
  html += "function esc(v){const d=document.createElement('div');d.textContent=v||'';return d.innerHTML}";
  html += "function escAttr(v){return esc(v).replace(/'/g,'&#39;')}";
  html += "async function loadSnapshots(){const list=await fetch('/snapshots').then(r=>r.json());snapshots.innerHTML=list.map(s=>`<tr><td>${esc(new Date(s.capturedAt).toLocaleString())}</td><td>${s.temperature.toFixed(2)} &deg;C</td><td>${s.humidity.toFixed(2)} %</td><td class='loc'>${s.hasLocation?`${s.latitude.toFixed(5)}, ${s.longitude.toFixed(5)}`:'Nije dostupna'}</td><td><input maxlength='95' value='${escAttr(s.comment)}' onchange='saveComment(${s.id},this.value)'></td></tr>`).join('');}";
  html += "function getLocation(){return new Promise(resolve=>{if(!navigator.geolocation)return resolve(null);navigator.geolocation.getCurrentPosition(p=>resolve(p.coords),()=>resolve(null),{enableHighAccuracy:false,timeout:5000,maximumAge:60000});});}";
  html += "async function captureSnapshot(){snapshotButton.disabled=true;message.textContent='Cuvam...';await refresh();if(!live||!live.ahtReadingValid){message.textContent='Nema ispravnog ocitavanja.';snapshotButton.disabled=false;return;}const loc=await getLocation();const p=new URLSearchParams({capturedAt:new Date().toISOString()});if(loc){p.set('lat',loc.latitude);p.set('lon',loc.longitude)}const r=await fetch('/snapshot',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});message.textContent=r.ok?'Snapshot je sacuvan.':'Cuvanje nije uspelo.';snapshotButton.disabled=false;await loadSnapshots();}";
  html += "async function saveComment(id,comment){const p=new URLSearchParams({id,comment});await fetch('/snapshot/comment',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});message.textContent='Komentar je sacuvan.';}";
  html += "setInterval(refresh,2000);refresh();loadSnapshots();";
  html += "</script></main></body></html>";
  debugServer.send(200, "text/html", html);
}

void handleDebugLogs() {
  debugServer.send(200, "text/plain", debugBuffer);
}

void handleDebugStatus() {
  char json[320];
  snprintf(json, sizeof(json),
           "{\"ahtPresent\":%s,\"ahtReadingValid\":%s,\"bqPresent\":%s,\"bqReadHealthy\":%s,\"lastTemperature\":%.2f,\"lastHumidity\":%.2f,\"lastBatteryV\":%.2f,\"intervalCitanjaMs\":%lu,\"i2cTimeoutMs\":%u,\"bqAdcDelayMs\":%u}",
           ahtPresent ? "true" : "false",
           ahtReadingValid ? "true" : "false",
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
    json += ",\"hasLocation\":" + String(item.hasLocation ? "true" : "false");
    json += ",\"latitude\":" + String(item.latitude, 6);
    json += ",\"longitude\":" + String(item.longitude, 6) + ",\"comment\":";
    appendJsonString(json, item.comment);
    json += '}';
  }
  json += ']';
  debugServer.send(200, "application/json", json);
}

void handleSnapshotCreate() {
  if (!ahtReadingValid || debugServer.arg("capturedAt").length() < 10) {
    debugServer.send(400, "text/plain", "Nema merenja ili vremena");
    return;
  }

  if (snapshotStore.count == MAX_SNAPSHOTS) {
    memmove(&snapshotStore.items[0], &snapshotStore.items[1], sizeof(Snapshot) * (MAX_SNAPSHOTS - 1));
    snapshotStore.count--;
  }

  Snapshot &item = snapshotStore.items[snapshotStore.count++];
  memset(&item, 0, sizeof(item));
  item.id = snapshotStore.nextId++;
  item.temperature = lastTemperature;
  item.humidity = lastHumidity;
  strlcpy(item.capturedAt, debugServer.arg("capturedAt").c_str(), sizeof(item.capturedAt));
  if (debugServer.hasArg("lat") && debugServer.hasArg("lon")) {
    item.latitude = debugServer.arg("lat").toDouble();
    item.longitude = debugServer.arg("lon").toDouble();
    item.hasLocation = true;
  }
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
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("==================================================");
  debugLogf("BOOT: VoiceToysWS pokrenut; Serial=115200 baud.");

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
  startWebOtaServer();
  loadSnapshots();
  debugLogf("Snapshot memorija: %u/%u sacuvano.", snapshotStore.count, MAX_SNAPSHOTS);

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
    debugLogf("Test VBAT: %.2fV", getBatteryVoltage());
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
      lastTemperature = t;
      lastHumidity = h;
      ahtReadingValid = true;
      debugLogf("Test AHT: T=%.2f H=%.2f", t, h);
    }
  }

  debugLogf("READY: inicijalizacija zavrsena; telemetrija na svakih %lu ms.", intervalCitanjaMs);
  Serial.println("==================================================");
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
      ahtReadingValid = false;
      if (millis() - lastAhtProbeMs >= 5000) {
        lastAhtProbeMs = millis();
        initAHT();
      }
      debugLogf("AHT citanje neuspesno.");
    } else {
      ahtReadingValid = true;
      Serial.printf("Temperatura: %.2f C | Vlaznost: %.2f %%\n", temperature, humidity);
    }

#if ENABLE_BQ
    float vbat = bqPresent ? getBatteryVoltage() : 0.0f;
#else
    float vbat = 0.0f;
#endif

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
