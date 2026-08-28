const fs = require('fs');
let code = fs.readFileSync('HTU21D.ino', 'utf8');

// 1. Add UUIDs and vars
code = code.replace(
  'NimBLECharacteristic *pDebugCharacteristic = nullptr;',
  'NimBLECharacteristic *pDebugCharacteristic = nullptr;\n' +
  'NimBLECharacteristic *pHistoryCharacteristic = nullptr;\n\n' +
  'bool historyStreamActive = false;\n' +
  'uint16_t historyStreamIndex = 0;\n' +
  'uint32_t historyStreamLogical = 0;\n' +
  'uint32_t historyStreamStride = 1;\n' +
  'uint32_t historyStreamOldest = 0;\n' +
  'bool historyStreamPersistent = false;\n' +
  'uint32_t historyStreamCount = 0;\n'
);
code = code.replace(
  '#define DEBUG_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"',
  '#define DEBUG_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"\n' +
  '#define HISTORY_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"'
);

// 2. Add history write callback
const callbacks = `
class MyHistoryCallbacks: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    std::string value = pChar->getValue();
    if (value.empty()) return;
    if (value[0] == 'G') { // "G" for GET_HISTORY
      uint32_t rangeSeconds = 86400; 
      historyStreamActive = true;
      historyStreamIndex = 0;
      historyStreamLogical = 0;
      if (!persistentHistoryReady || persistentHistory.count == 0) {
        historyStreamPersistent = false;
        historyStreamCount = historyCount;
        historyStreamStride = historyCount > 600 ? (historyCount + 599) / 600 : 1;
      } else {
        historyStreamPersistent = true;
        uint32_t req = min(persistentHistory.count, rangeSeconds);
        historyStreamStride = (req + 599) / 600;
        historyStreamOldest = (persistentHistory.head + persistentHistory.capacity - persistentHistory.count) % persistentHistory.capacity;
        historyStreamLogical = persistentHistory.count - req;
        historyStreamCount = persistentHistory.count;
      }
      debugLogf("BLE History Stream pokrenut.");
    }
  }
};
`;

code = code.replace('class MyServerCallbacks: public NimBLEServerCallbacks {', callbacks + '\nclass MyServerCallbacks: public NimBLEServerCallbacks {');

// 3. Update ServerCallbacks (WiFi toggle)
code = code.replace(/void onConnect\(NimBLEServer\* pServer, ble_gap_conn_desc\* desc\) override \{[\s\S]*?pServer->updateConnParams[^\n]*\n\s*\}/m,
`void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
    deviceConnected = true;
    debugLogf("Klijent spojen.");
    pServer->updateConnParams(desc->conn_handle, 24, 40, 0, 200);
    
    bool charging = false;
    if (bqPresent) {
      uint8_t reg0b = readBQRegister(0x0B);
      charging = ((reg0b & 0x18) != 0);
    }
    if (!charging && WiFi.status() != WL_CONNECTED && !otaServerActive) {
      debugLogf("BLE spojen: baterija se ne puni i nema poznate mreze. Gasim WiFi radi ustede.");
      WiFi.mode(WIFI_OFF);
      wifiApFallbackActive = false;
    }
  }`);
  
code = code.replace(/void onDisconnect\(NimBLEServer\* pServer\) override \{[\s\S]*?pServer->getAdvertising\(\)->start\(\);\n\s*\}/m,
`void onDisconnect(NimBLEServer* pServer) override {
    deviceConnected = false;
    historyStreamActive = false;
    debugLogf("Klijent otkacen. Ponovno oglasavanje...");
    pServer->getAdvertising()->start();
    
    if (WiFi.getMode() == WIFI_OFF && !otaServerActive) {
      debugLogf("BLE otkacen: Vracam mrezu (AP fallback)...");
      networkReconfigurePending = true;
      networkReconfigureAtMs = millis() + 500;
    }
  }`);

// 4. Create History Characteristic
code = code.replace(/pService->start\(\);/m,
`pHistoryCharacteristic = pService->createCharacteristic(
                      HISTORY_CHARACTERISTIC_UUID,
                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                    );
  pHistoryCharacteristic->setCallbacks(new MyHistoryCallbacks());
  pService->start();`);

fs.writeFileSync('HTU21D.ino', code);
