const fs = require('fs');
let code = fs.readFileSync('app/main.js', 'utf8');

code = "import { isNative, initBle, connectBle, sendCommand, deviceId } from './BleController.js';\n" + code;

// Modify refresh()
code = code.replace(/async function refresh\(\) \{[\s\S]*?\}\s*function showTab/m,
`async function refresh() {
  if (isNative) {
    if (!deviceId) {
      sensorState.textContent = 'Klikni na BLE ikonicu za povezivanje';
      return;
    }
    // Ocitavanje se obnavlja asinhrono kroz onLiveUpdate.
    return;
  }
  
  try {
    live = await fetch("/status", { cache: "no-store" }).then((r) => r.json());
    const ok = live.ahtReadingValid;
    temperature.textContent = ok ? live.lastTemperature.toFixed(2) : "--";
    humidity.textContent = ok ? live.lastHumidity.toFixed(2) : "--";
    realFeel.textContent = ok ? live.lastRealFeel.toFixed(2) : "--";
    absoluteHumidity.textContent = ok ? live.lastAbsoluteHumidity.toFixed(2) : "--";
    if (ok) {
      setTrend("temperature", live.temperatureRate, 18, "C");
      setTrend("humidity", live.humidityRate, 5, "%");
      setTrend("realFeel", live.realFeelRate, 18, "C");
      setTrend("absoluteHumidity", live.absoluteHumidityRate, 14, "g/m3");
    }
    sensorState.textContent = ok ? "Senzor je aktivan · osvezavanje 0.5 s" : live.ahtPresent ? "Ceka se prvo merenje" : "Senzor nije pronadjen";
  } catch (e) {
    sensorState.textContent = "Nema veze sa uredjajem";
  }
}
function showTab`);

// Modify refreshCharts()
code = code.replace(/async function refreshCharts\(\) \{[\s\S]*?\}\s*function setCursorFromEvent/m,
`async function refreshCharts() {
  if (isNative) {
    if (deviceId) {
      // Send command to pull history over BLE
      sendCommand('G');
    }
    return;
  }
  
  try {
    const userDragging = document.activeElement === rangeStart || document.activeElement === rangeEnd;
    if (userDragging) liveRangeFollow = false;
    const oldWindow = rangeReady ? Math.max(1, Number(rangeEnd.value) - Number(rangeStart.value)) : 0;
    const wasAtEnd = rangeReady && Number(rangeEnd.value) >= Math.max(0, allChartData.length - 1);
    const d = await fetch("/history?range=86400", { cache: "no-store" }).then((r) => r.json());
    if (!d.length) return;
    allChartData = d;
    updateChartRange(oldWindow, wasAtEnd);
  } catch (e) {}
}

function updateChartRange(oldWindow, wasAtEnd) {
    const max = allChartData.length - 1;
    rangeStart.max = rangeEnd.max = max;
    if (!rangeReady) {
      rangeStart.value = 0;
      rangeEnd.value = max;
      rangeReady = true;
      liveRangeFollow = true;
      rangeWindowSize = max;
    } else if (wasAtEnd || liveRangeFollow) {
      const windowSize = Math.max(1, oldWindow || 60);
      rangeStart.value = Math.max(0, max - windowSize);
      rangeEnd.value = max;
      liveRangeFollow = true;
      rangeWindowSize = windowSize;
    } else {
      rangeWindowSize = Math.max(1, rangeWindowSize || oldWindow || 1);
      rangeStart.value = Math.min(Number(rangeStart.value), max);
      rangeEnd.value = Math.min(Math.max(Number(rangeEnd.value), Number(rangeStart.value)), max);
    }
    applyTimeRange();
}

function setCursorFromEvent`);

// Modify setup code at bottom
code = code.replace(/setInterval\(refresh, 500\);[\s\S]*?loadSnapshots\(\);/,
`setInterval(refresh, 500);
if (!isNative) {
  setInterval(refreshCharts, 5000);
}

refresh();
if (!isNative) refreshCharts();
if (!isNative) loadSnapshots();

if (isNative) {
  const header = document.querySelector('header');
  const bleBtn = document.createElement('button');
  bleBtn.textContent = 'Poveži BLE';
  bleBtn.style.marginLeft = '10px';
  bleBtn.onclick = async () => {
    bleBtn.textContent = 'Povezujem...';
    let ok = await connectBle();
    bleBtn.textContent = ok ? 'Povezan' : 'Poveži BLE';
    if(ok) {
       sensorState.textContent = 'BLE Povezan! Povlacim podatke...';
       sendCommand('G'); // Request history
    }
  };
  header.appendChild(bleBtn);
  
  // hide HTTP specific tabs
  $('snapTab').style.display = 'none';
  $('networkTab').style.display = 'none';
  $('debugTab').style.display = 'none';
  
  initBle((str) => {
    // onLiveUpdate (T:XX.XX,H:XX.XX,V:XX.XXV)
    let tMatch = str.match(/T:([0-9.-]+)/);
    let hMatch = str.match(/H:([0-9.-]+)/);
    let vMatch = str.match(/V:([0-9.-]+)/);
    if(tMatch && hMatch) {
       let t = parseFloat(tMatch[1]);
       let h = parseFloat(hMatch[1]);
       let d = derived(t, h);
       live = {
         ahtReadingValid: true,
         lastTemperature: t,
         lastHumidity: h,
         lastRealFeel: d.r,
         lastAbsoluteHumidity: d.a,
         temperatureRate: 0,
         humidityRate: 0,
         realFeelRate: 0,
         absoluteHumidityRate: 0
       };
       sensorState.textContent = "Senzor je aktivan (BLE)";
       
       temperature.textContent = t.toFixed(2);
       humidity.textContent = h.toFixed(2);
       realFeel.textContent = d.r.toFixed(2);
       absoluteHumidity.textContent = d.a.toFixed(2);
    }
  }, (buf) => {
    // onHistoryUpdate (binary chunk)
    const dv = new DataView(buf);
    const numPoints = dv.byteLength / 8;
    for(let i = 0; i < numPoints; i++) {
       let s = dv.getUint32(i*8, true); // little endian
       let t100 = dv.getInt16(i*8 + 4, true);
       let h100 = dv.getInt16(i*8 + 6, true);
       
       if (s === 0xFFFFFFFF) {
         // EOF
         console.log("History EOF received. Total:", allChartData.length);
         const userDragging = document.activeElement === rangeStart || document.activeElement === rangeEnd;
         if (userDragging) liveRangeFollow = false;
         const oldWindow = rangeReady ? Math.max(1, Number(rangeEnd.value) - Number(rangeStart.value)) : 0;
         const wasAtEnd = rangeReady && Number(rangeEnd.value) >= Math.max(0, allChartData.length - 1);
         updateChartRange(oldWindow, wasAtEnd);
         break;
       }
       
       let t = t100 / 100.0;
       let h = h100 / 100.0;
       let d = derived(t, h);
       allChartData.push({s, t, h, r: d.r, a: d.a});
    }
    // Sort just in case
    allChartData.sort((a,b) => a.s - b.s);
  });
  
  // Inject OTA button on UI
  const actionDiv = document.querySelector('.actions');
  const otaBtn = document.createElement('button');
  otaBtn.className = 'update';
  otaBtn.textContent = 'Upali WiFi i OTA Server';
  otaBtn.onclick = () => {
    sendCommand('U');
    alert('OTA server se pali, povezi telefon na ws.local:8080/webota ili AP VoiceToysWS-OTA');
  };
  actionDiv.appendChild(otaBtn);
}
`);

fs.writeFileSync('app/main.js', code);
