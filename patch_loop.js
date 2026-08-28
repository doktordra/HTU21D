const fs = require('fs');
let code = fs.readFileSync('HTU21D.ino', 'utf8');

code = code.replace(/void loop\(\) \{\s*if \(historyStreamActive && deviceConnected\) \{/,
`void loop() {
  static uint32_t lastHistoryChunkMs = 0;
  if (historyStreamActive && deviceConnected && (millis() - lastHistoryChunkMs > 30)) {
    lastHistoryChunkMs = millis();`);
    
fs.writeFileSync('HTU21D.ino', code);
