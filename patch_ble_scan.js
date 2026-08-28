const fs = require('fs');
let code = fs.readFileSync('app/BleController.js', 'utf8');

code = code.replace(/export async function connectBle\(\) \{[\s\S]*?console\.log\("Povezan sa", deviceId\);/m,
`export async function connectBle() {
  try {
    let foundDeviceId = null;
    await BleClient.requestLEScan({ services: [SERVICE_UUID] }, (result) => {
      console.log("Pronadjen uredjaj:", result.device.name);
      if (result.device.name === 'VoiceToysWS' || result.device.name === 'VoiceToysWS-OTA') {
        foundDeviceId = result.device.deviceId;
      }
    });
    
    // Sacekaj max 5 sekundi
    for(let i = 0; i < 50; i++) {
      if (foundDeviceId) break;
      await new Promise(r => setTimeout(r, 100));
    }
    
    await BleClient.stopLEScan();
    
    if (!foundDeviceId) {
       console.log("Nije pronadjen VoiceToysWS!");
       return false;
    }
    
    deviceId = foundDeviceId;
    
    await BleClient.connect(deviceId, (dId) => {
      console.log("Disconnected", dId);
      deviceId = null;
    });
    
    console.log("Povezan sa", deviceId);`);

fs.writeFileSync('app/BleController.js', code);
