const fs = require('fs');
let code = fs.readFileSync('app/main.js', 'utf8');

// 1. Do not hide debugTab
code = code.replace("$('debugTab').style.display = 'none';", "");

// 2. Output "Gotovo" on EOF
code = code.replace('console.log("History EOF received. Total:", allChartData.length);', 'console.log("History EOF received. Total:", allChartData.length);\n         sensorState.textContent = "Istorija učitana! (" + allChartData.length + " tačaka)";\n         setTimeout(() => { sensorState.textContent = "Senzor je aktivan (BLE)"; }, 3000);');

// 3. Add WiFi toggle in UI
code = code.replace("actionDiv.appendChild(otaBtn);", 
`actionDiv.appendChild(otaBtn);
  
  const debugGrid = document.querySelector('.debugGrid');
  if (debugGrid) {
    const wifiBtn = document.createElement('button');
    wifiBtn.className = 'update';
    wifiBtn.style.marginTop = '10px';
    wifiBtn.textContent = 'Prekidač za WiFi (ON/OFF)';
    wifiBtn.onclick = () => {
      sendCommand('W');
      alert('Poslata komanda za WiFi.');
    };
    document.getElementById('debugWifi').parentNode.appendChild(wifiBtn);
  }`);

fs.writeFileSync('app/main.js', code);
