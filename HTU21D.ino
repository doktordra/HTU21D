#include <Wire.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebOTA.h>

// I2C 0: HTU-21D Senzor (SDA=15, SCL=2)
#define AHT_SDA 15
#define AHT_SCL 2
#define HTU21D_ADDRESS 0x40

// I2C 1: BQ25895 PMIC (SDA=33, SCL=13)
#define BQ_SDA 33
#define BQ_SCL 13
#define BQ25895_ADDRESS 0x6A

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

NimBLECharacteristic *pCharacteristic;
bool deviceConnected = false;
bool otaServerActive = false;
bool otaStartRequested = false;

const char OTA_TRIGGER_CHAR = 'U';
const char OTA_AP_SSID[] = "VoiceToysWS-OTA";

// Tajmer za čitanje senzora i slanje (3 sekunde)
unsigned long zadnjeVremeCitanja = 0;
const unsigned long INTERVAL_CITANJA = 3000; 

class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override { 
        deviceConnected = true; 
        Serial.println("Klijent spojen.");
        // Postavljanje stabilnog intervala konekcije
        pServer->updateConnParams(desc->conn_handle, 24, 40, 0, 200);
    };
    
    void onDisconnect(NimBLEServer* pServer) override {
        deviceConnected = false;
        Serial.println("Klijent otkačen. Ponovno oglašavanje...");
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
          Serial.println("BLE komanda primljena: pokretanje OTA servera.");
        }
    }

  void onWrite(NimBLECharacteristic* pCharacteristic) {
    handleWrite(pCharacteristic);
  }

  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    handleWrite(pCharacteristic);
  }
};

// --- BQ25895 Funkcije ---
void writeBQRegister(uint8_t reg, uint8_t value) {
  Wire1.beginTransmission(BQ25895_ADDRESS);
  Wire1.write(reg);
  Wire1.write(value);
  Wire1.endTransmission();
}

uint8_t readBQRegister(uint8_t reg) {
  Wire1.beginTransmission(BQ25895_ADDRESS);
  Wire1.write(reg);
  Wire1.endTransmission(false);
  Wire1.requestFrom(BQ25895_ADDRESS, 1);
  if (Wire1.available()) return Wire1.read();
  return 0;
}

void initBQ25895() {
  uint8_t reg05 = readBQRegister(0x05);
  reg05 &= ~(0x30); // Isključi Watchdog za stalno
  writeBQRegister(0x05, reg05);

  uint8_t reg02 = readBQRegister(0x02);
  reg02 |= (1 << 7) | (1 << 6); // ADC kontinualno merenje
  writeBQRegister(0x02, reg02);

  uint8_t reg03 = readBQRegister(0x03);
  reg03 &= ~(0x30); 
  reg03 |= 0x20;    // OTG Boost 5V
  writeBQRegister(0x03, reg03);
}

float getBatteryVoltage() {
  uint8_t reg0E = readBQRegister(0x0E);
  return 2.304 + ((reg0E & 0x7F) * 0.020);
}

bool readHTU21DTemperatureHumidity(float &temperature, float &humidity) {
  Wire.beginTransmission(HTU21D_ADDRESS);
  Wire.write(0xF5);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  delay(30);

  Wire.requestFrom(HTU21D_ADDRESS, 3);
  if (Wire.available() != 3) {
    return false;
  }

  uint16_t rawHumidity = ((uint16_t)Wire.read() << 8) | Wire.read();
  Wire.read();
  rawHumidity &= 0xFFFC;
  humidity = -6.0 + (125.0 * ((float)rawHumidity / 65536.0));

  Wire.beginTransmission(HTU21D_ADDRESS);
  Wire.write(0xF3);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  delay(30);

  Wire.requestFrom(HTU21D_ADDRESS, 3);
  if (Wire.available() != 3) {
    return false;
  }

  uint16_t rawTemperature = ((uint16_t)Wire.read() << 8) | Wire.read();
  Wire.read();
  rawTemperature &= 0xFFFC;
  temperature = -46.85 + (175.72 * ((float)rawTemperature / 65536.0));

  return true;
}

void startWebOtaServer() {
  if (otaServerActive) {
    Serial.println("OTA server je vec aktivan.");
    return;
  }

  Serial.println("Pokrecem WiFi AP i WebOTA server...");
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(OTA_AP_SSID)) {
    Serial.println("Greska pri pokretanju softAP mreze.");
    return;
  }

  IPAddress apIp = WiFi.softAPIP();
  Serial.print("OTA AP IP adresa: ");
  Serial.println(apIp);

  if (webota.init(8080, "/webota")) {
    otaServerActive = true;
  } else {
    Serial.println("Greska pri pokretanju WebOTA servera.");
    return;
  }

  Serial.println("WebOTA spreman. Otvori /webota u browseru preko AP mreze.");
}

void setup() {
  // Postavljanje bezbednog, a štedljivog clock-a za NimBLE
  setCpuFrequencyMhz(80);
  
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF); // Wi-Fi ostaje ugašen dok BLE ne okine OTA

  Serial.println("--- VoiceToysWS: Stabilan rad na 80MHz ---");
  Serial.printf("BLE OTA trigger: posalji ASCII karakter '%c'.\n", OTA_TRIGGER_CHAR);

  // Inicijalizacija I2C za HTU-21D
  Wire.begin(AHT_SDA, AHT_SCL);
  delay(40);
  Wire.beginTransmission(HTU21D_ADDRESS);
  Wire.write(0xFE);
  Wire.endTransmission();
  delay(20);

  // Inicijalizacija I2C za BQ25895
  Wire1.begin(BQ_SDA, BQ_SCL);
  delay(40);
  initBQ25895();

  // Inicijalizacija NimBLE steka
  NimBLEDevice::init("VoiceToysWS");
  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                    );
  pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());

  pService->start();
  
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  Serial.println("Uređaj spreman i čeka konekciju...");
}

void loop() {
  if (otaStartRequested) {
    otaStartRequested = false;
    startWebOtaServer();
  }

  if (otaServerActive) {
    webota.handle();
  }

  // Izvršava se samo kada prođe zadati interval (svake 3 sekunde)
  if (millis() - zadnjeVremeCitanja >= INTERVAL_CITANJA) {
    zadnjeVremeCitanja = millis();

    float temperature = 0.0;
    float humidity = 0.0;

    // --- Čitanje HTU-21D ---
    if (!readHTU21DTemperatureHumidity(temperature, humidity)) {
      Serial.println("Greska pri citanju HTU-21D senzora.");
    }

    // --- Čitanje BQ25895 ---
    float vbat = getBatteryVoltage();

    // --- Formatiranje ASCII stringa ---
    char asciiBuffer[64];
    snprintf(asciiBuffer, sizeof(asciiBuffer), "T:%.2f,H:%.2f,V:%.2fV", temperature, humidity, vbat);

    Serial.print("Status: ");
    Serial.println(asciiBuffer);

    // Ažuriranje vrednosti karakteristike
    pCharacteristic->setValue((uint8_t*)asciiBuffer, strlen(asciiBuffer));

    // Slanje notifikacije ako je klijent spojen
    if (deviceConnected) {
      pCharacteristic->notify();
      Serial.println("Notifikacija poslata klijentu.");
    }
  }

  // BLE i OTA server ostaju responsivni, loop i dalje ostaje lagan
  delay(10); 
}
