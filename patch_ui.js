const fs = require('fs');

// Create a clean index.html with the right CSS and tab placement
const html = `<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>VoiceToys Weather Station</title>
<style>
:root{--ink:#17211c;--paper:#f4f1e8;--accent:#087e6a;--line:#c9c7ba;--up:#14834b;--down:#c43b32;--flat:#d2a400}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font-family:Georgia,serif;padding-bottom:60px;}
main{width:min(900px,calc(100% - 24px));margin:12px auto}header{display:flex;justify-content:space-between;align-items:end;border-bottom:2px solid var(--ink);padding-bottom:8px;gap:10px}
h1{font-size:20px;margin:0}.version{font-family:sans-serif;font-size:12px;color:#59635d;margin-top:2px}
.tabs,.actions{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.view{display:none}.view.active{display:block}
.live{display:grid;gap:12px;margin:14px 0 22px}.metric{display:grid;grid-template-columns:minmax(190px,220px) 140px minmax(0,1fr);align-items:center;column-gap:8px;position:relative;border:1px solid var(--line);padding:12px;background:#fff;min-height:120px}.reading{align-self:center}.metricName{font-size:15px;font-weight:700;color:#445149;margin-bottom:2px}.value{font:700 clamp(28px,6vw,40px) Georgia,serif}.unit{font-size:15px;color:#59635d}
.trendBox{display:grid;grid-template-columns:78px 58px;align-items:center;justify-content:start;gap:6px;min-width:140px;padding-right:14px}.trendVisual{display:grid;grid-template-rows:82px auto;justify-items:center;align-items:center}.rangeStack{height:112px;display:flex;flex-direction:column;justify-content:space-between;align-items:flex-start}.rangeLabel{font:700 16px Arial,sans-serif;color:#35443c;white-space:nowrap}.rangeDelta{font-size:14px;color:#59665f}.rangeDelta.up{color:var(--up)}.rangeDelta.down{color:var(--down)}.metric .rate{font:700 14px Arial,sans-serif;color:#45524a;white-space:nowrap;text-align:center;margin-top:3px}.trend{position:relative;width:34px;height:78px;color:var(--flat);transform:rotate(0deg);transition:transform .45s cubic-bezier(.2,.8,.2,1),color .35s ease}.trend .shaft{position:absolute;left:15px;bottom:9px;width:4px;height:var(--len,0px);max-height:56px;background:currentColor;border-radius:4px;transition:height .55s cubic-bezier(.2,.8,.2,1)}.trend .shaft:before{content:'';position:absolute;left:-5px;top:-3px;border-left:7px solid transparent;border-right:7px solid transparent;border-bottom:10px solid currentColor;transform:translateY(-7px)}.trend.down{color:var(--down);transform:rotate(180deg)}.trend.up{color:var(--up)}.trend.flat{color:var(--flat);transform:rotate(90deg)}.trend.flat .shaft{height:25px!important}.trend.flat .shaft:before{display:block}.chart{min-width:0}.chart canvas{display:block;width:100%;height:100px;touch-action:none}
button{border:0;background:var(--accent);color:#fff;padding:11px 16px;font-weight:700;cursor:pointer}button:disabled{opacity:.5}.update{background:#263d70}.actions{margin:12px 0 22px}.note{font-size:13px;color:#59635d}
.timeline{background:#fff;border:1px solid var(--line);padding:14px 18px;margin:10px 0 14px}.timelineTrack{position:relative;height:28px;margin:2px 5px}.timelineRail,.timelineFill{position:absolute;left:0;right:0;top:12px;height:5px;border-radius:5px;background:#d4d5cf}.timelineFill{right:auto;background:var(--accent)}.timeline input[type=range]{position:absolute;left:0;top:0;width:100%;height:28px;margin:0;padding:0;border:0;background:transparent;pointer-events:none;appearance:none}.timeline input[type=range]::-webkit-slider-thumb{appearance:none;width:22px;height:22px;border-radius:50%;background:#fff;border:4px solid var(--accent);box-shadow:0 1px 4px #0005;pointer-events:auto;cursor:grab}.timeline input[type=range]::-moz-range-thumb{width:15px;height:15px;border-radius:50%;background:#fff;border:4px solid var(--accent);box-shadow:0 1px 4px #0005;pointer-events:auto;cursor:grab}.timelineLabels{display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:10px;font:700 13px Arial,sans-serif;color:#46534c}.timelineLabels span:nth-child(2){text-align:center}.timelineLabels span:last-child{text-align:right}.snapshotReading{min-height:16px;margin-top:5px;color:#8a6320;font:12px Arial,sans-serif}.save{background:#9b6516}.hidden{display:none!important}.deleteHint{color:#8e4038}.snapshotRow{touch-action:manipulation}
table{width:100%;border-collapse:collapse;background:#fff}th,td{text-align:left;border-bottom:1px solid var(--line);padding:10px;vertical-align:top}th{font-size:12px;text-transform:uppercase}
input,select{width:100%;min-width:140px;padding:8px;border:1px solid var(--line);font:14px Georgia,serif}.networkCard{background:#fff;border:1px solid var(--line);padding:16px;margin:12px 0}.networkCard h2{font-size:18px;margin:0 0 10px}.networkForm{display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:end}.networkList{display:grid;gap:7px;margin-top:10px}.networkItem{display:flex;justify-content:space-between;align-items:center;gap:8px;border-bottom:1px solid var(--line);padding:7px 0}.danger{background:#9d332c}.address{font-family:Arial,sans-serif;font-weight:700;word-break:break-all}.debugGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}.debugValue{font:700 20px Arial,sans-serif}.ok{color:var(--up)}.bad{color:var(--down)}.switchLine{display:flex;align-items:center;gap:10px}.switchLine input{width:auto;min-width:0}#debugLogs{display:block;white-space:pre-wrap;overflow-wrap:anywhere;max-height:320px;overflow:auto;background:#18201c;color:#dce8df;padding:12px;font:11px monospace}#message,#networkMessage,#debugMessage{min-height:20px}

/* Sticky tabs at the bottom */
.tabs{position:fixed;bottom:0;left:0;right:0;background:var(--paper);display:flex;margin:0;padding:0;border-top:2px solid var(--ink);z-index:100;}
.tab{flex:1;background:transparent;color:var(--ink);border:none;padding:14px 10px;font-size:16px;border-right:1px solid var(--line);}
.tab:last-child{border-right:none;}
.tab.active{background:var(--accent);color:#fff;}

@media(max-width:620px){
  body{padding-bottom:50px;} /* space for tabs */
  main{margin:6px auto}
  .metric{grid-template-columns:minmax(0,1fr) 130px;padding:8px;column-gap:4px;min-height:0;margin-bottom:6px}
  .metricName{font-size:14px;font-weight:600}
  .value{font-size:26px}
  .unit{font-size:14px}
  .trendBox{grid-column:2;grid-row:1;min-width:130px;}
  .rangeStack{height:80px}
  .trend{height:70px}
  .trendVisual{grid-template-rows:70px auto}
  .chart{grid-column:1/-1;margin-top:2px}
  .chart canvas{height:70px}
  .timeline{padding:6px}
  .timelineLabels{font-size:11px;gap:5px}
  header h1{font-size:18px;margin:0}
  header{align-items:start;flex-direction:column;gap:2px;padding:6px}
  .networkForm{grid-template-columns:1fr}
  .networkForm button{width:100%}
  table,thead,tbody,tr,th,td{display:block}
  thead{display:none}
  tr{border-bottom:2px solid var(--ink)}
  td{border:0;padding:6px 10px}
  .tab{padding:12px 6px;font-size:14px}
}
</style></head><body><main>
<header>
  <div>
    <h1>VoiceToys Weather Station</h1>
    <div class="version">App verzija: 1.1.0</div>
  </div>
  <span id='sensorState' class='note'>Povezivanje...</span>
</header>
<section id='liveView' class='view active'><div class='timeline'><div class='timelineTrack'><div class='timelineRail'></div><div id='timelineFill' class='timelineFill'></div><input id='rangeStart' type='range' min='0' max='1' value='0' oninput='applyTimeRange()'><input id='rangeEnd' type='range' min='0' max='1' value='1' oninput='applyTimeRange()'></div><div class='timelineLabels'><span id='recordedFrom'>--:--:--</span><span id='recordedDate'>--/--/----</span><span id='recordedTo'>--:--:--</span></div></div><section class='live'>
<div class='metric'><div class='reading'><div class='metricName'>Temperatura</div><span id='temperature' class='value'>--</span><span class='unit'> &deg;C</span><div id='temperatureSnapshot' class='snapshotReading'></div></div><div class='trendBox'><div class='trendVisual'><div id='temperatureTrend' class='trend flat'><i class='shaft'></i></div><div id='temperatureRate' class='rate'>--</div></div><div class='rangeStack'><span id='temperatureMax' class='rangeLabel'>--</span><span id='temperatureDelta' class='rangeLabel rangeDelta'>--</span><span id='temperatureMin' class='rangeLabel'>--</span></div></div><div class='chart'><canvas id='temperatureChart'></canvas></div></div>
<div class='metric'><div class='reading'><div class='metricName'>Relativna vlaznost</div><span id='humidity' class='value'>--</span><span class='unit'> %</span><div id='humiditySnapshot' class='snapshotReading'></div></div><div class='trendBox'><div class='trendVisual'><div id='humidityTrend' class='trend flat'><i class='shaft'></i></div><div id='humidityRate' class='rate'>--</div></div><div class='rangeStack'><span id='humidityMax' class='rangeLabel'>--</span><span id='humidityDelta' class='rangeLabel rangeDelta'>--</span><span id='humidityMin' class='rangeLabel'>--</span></div></div><div class='chart'><canvas id='humidityChart'></canvas></div></div>
<div class='metric'><div class='reading'><div class='metricName'>RealFeel (procena)</div><span id='realFeel' class='value'>--</span><span class='unit'> &deg;C</span><div id='realFeelSnapshot' class='snapshotReading'></div></div><div class='trendBox'><div class='trendVisual'><div id='realFeelTrend' class='trend flat'><i class='shaft'></i></div><div id='realFeelRate' class='rate'>--</div></div><div class='rangeStack'><span id='realFeelMax' class='rangeLabel'>--</span><span id='realFeelDelta' class='rangeLabel rangeDelta'>--</span><span id='realFeelMin' class='rangeLabel'>--</span></div></div><div class='chart'><canvas id='realFeelChart'></canvas></div></div>
<div class='metric'><div class='reading'><div class='metricName'>Apsolutna vlaznost</div><span id='absoluteHumidity' class='value'>--</span><span class='unit'> g/m&sup3;</span><div id='absoluteHumiditySnapshot' class='snapshotReading'></div></div><div class='trendBox'><div class='trendVisual'><div id='absoluteHumidityTrend' class='trend flat'><i class='shaft'></i></div><div id='absoluteHumidityRate' class='rate'>--</div></div><div class='rangeStack'><span id='absoluteHumidityMax' class='rangeLabel'>--</span><span id='absoluteHumidityDelta' class='rangeLabel rangeDelta'>--</span><span id='absoluteHumidityMin' class='rangeLabel'>--</span></div></div><div class='chart'><canvas id='absoluteHumidityChart'></canvas></div></div>
</section>
<div class='actions'><button id='snapshotButton' onclick='captureSnapshot()'>Napravi snapshot</button><button id='saveSnapshotButton' class='save hidden' onclick='savePendingSnapshot()'>Sacuvaj snapshot</button><span id='message' class='note'></span></div></section>
<section id='snapView' class='view'><p class='note'>Snapshotovi su trajno sacuvani u ESP32 memoriji (NVS), najvise 24. <span class='deleteHint'>Drzi red oko 1 sekunde da ga obrises.</span></p><table><thead><tr><th>Vreme</th><th>Temperatura</th><th>Rel. vlaznost</th><th>RealFeel</th><th>Aps. vlaznost</th><th>Komentar</th></tr></thead><tbody id='snapshots'></tbody></table></section>
<section id='networkView' class='view'><div class='networkCard'><h2>Status mreze</h2><div id='networkState'>Ucitavanje...</div><p class='note'>Na kucnoj mrezi otvori:</p><div class='address'>http://ws.local:8081/</div></div><div class='networkCard'><h2>Dodaj ili promeni mrezu</h2><p class='note'>Izaberi skeniranu mrezu ili upisi SSID. Lozinka se cuva samo u ESP32 NVS memoriji.</p><div class='networkForm'><label>Wi-Fi mreza<input id='networkSsid' list='availableNetworks' maxlength='32' placeholder='SSID'><datalist id='availableNetworks'></datalist></label><label>Lozinka<input id='networkPassword' type='password' maxlength='64' autocomplete='new-password' placeholder='Prazno za otvorenu mrezu'></label><button onclick='saveNetwork()'>Sacuvaj</button></div><div class='actions'><button onclick='scanNetworks()'>Pretrazi mreze</button><span id='networkMessage' class='note'></span></div></div><div class='networkCard'><h2>Zapamcene mreze</h2><div id='knownNetworks' class='networkList'></div></div><div class='networkCard'><h2>Firmware</h2><p class='note'>Update radi i preko kucne mreze i preko fallback AP mreze.</p><button class='update' onclick='openUpdate()'>Update firmware</button></div></section>
<section id='debugView' class='view'><div class='debugGrid'><div class='networkCard'><h2>AHT senzor</h2><div id='debugSensor' class='debugValue'>--</div><div class='note'>I2C SDA 1 / SCL 3</div></div><div class='networkCard'><h2>Baterija / BQ25895</h2><div id='debugBattery' class='debugValue'>--</div><div id='debugBq' class='note'>--</div></div><div class='networkCard'><h2>BLE</h2><div id='debugBle' class='debugValue'>--</div></div><div class='networkCard'><h2>WebOTA</h2><div id='debugOta' class='debugValue'>--</div></div><div class='networkCard'><h2>Wi-Fi signal</h2><div id='debugWifi' class='debugValue'>--</div></div><div class='networkCard'><h2>Sistem</h2><div id='debugSystem' class='debugValue'>--</div></div></div><div class='networkCard'><h2>RGB dijagnostika</h2><label class='switchLine'><input id='diagnosticLedToggle' type='checkbox' onchange='setDiagnosticLeds(this.checked)'>Kratak impuls na svake 3 sekunde</label><p class='note'>LED 1: baterija zeleno / zuto / crveno. LED 2: AHT senzor zeleno / crveno. Osvetljenje je ograniceno radi stednje baterije.</p><span id='debugMessage' class='note'></span></div><div class='networkCard'><h2>Debug log</h2><button onclick='loadDebug()'>Osvezi</button><pre id='debugLogs'>--</pre></div></section>

<!-- TABS MOVED TO BOTTOM -->
<nav class='tabs'><button id='liveTab' class='tab active' onclick="showTab('live')">Uzivo</button><button id='snapTab' class='tab' onclick="showTab('snap')">Snapshotovi</button><button id='networkTab' class='tab' onclick="showTab('network')">Network</button><button id='debugTab' class='tab' onclick="showTab('debug')">Debug</button></nav>

<script type="module" src="/main.js"></script></main></body></html>`;

fs.writeFileSync('app/index.html', html);
