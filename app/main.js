import { isNative, initBle, connectBle, sendCommand, requestHistory, interpolateHistory, sendTime, setWifiEnabled, deviceId, forceDisconnect } from './BleController.js';
let live = null;
const $ = (id) => document.getElementById(id);
function setTrend(name, rate, scale, unit) {
  const el = $(name + "Trend"),
    out = $(name + "Rate"),
    a = Math.abs(rate),
    flat = a < 0.005;
  el.className = "trend " + (flat ? "flat" : rate > 0 ? "up" : "down");
  el.style.setProperty("--len", Math.min(56, 12 + a * scale) + "px");
  out.textContent = flat
    ? "stabilno"
    : (rate > 0 ? "+" : "") +
      rate.toFixed(a < 0.1 ? 2 : 1) +
      " " +
      unit +
      "/min";
}
function updateBatteryUI(voltage) {
  const fill = $("batteryFill"), pct = $("batteryPct");
  if (!fill || !pct) return;
  if (!voltage || voltage <= 0) {
    fill.style.width = "0%";
    pct.textContent = "--";
    return;
  }
  const ratio = clamp((voltage - 3.0) / (4.2 - 3.0), 0, 1);
  const percent = Math.round(ratio * 100);
  fill.style.width = percent + "%";
  fill.style.background = voltage >= 3.75 ? "var(--up)" : voltage >= 3.45 ? "var(--flat)" : "var(--down)";
  pct.textContent = percent + "%";
}
async function refresh() {
  if (isNative) {
    // Ocitavanje se obnavlja asinhrono kroz onLiveUpdate; sensorState prati auto-konekciju.
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
    updateBatteryUI(live.lastBatteryV);
  } catch (e) {
    sensorState.textContent = "Nema veze sa uredjajem";
  }
}
function showTab(t) {
  liveView.classList.toggle("active", t === "live");
  snapView.classList.toggle("active", t === "snap");
  networkView.classList.toggle("active", t === "network");
  debugView.classList.toggle("active", t === "debug");
  liveTab.classList.toggle("active", t === "live");
  snapTab.classList.toggle("active", t === "snap");
  networkTab.classList.toggle("active", t === "network");
  debugTab.classList.toggle("active", t === "debug");
  if (t === "snap") loadSnapshots();
  if (t === "network") loadNetwork();
  if (t === "debug") loadDebug();
}
function openUpdate() {
  location.href = "http://" + location.hostname + ":8080/webota";
}
let allChartData = [],
  sliderData = [],
  chartData = [],
  cursorIndex = -1,
  rangeReady = false,
  rangeWindowSize = 0,
  liveRangeFollow = true,
  sliderStart = 0,
  sliderEnd = 0,
  activeSelectedRange = null, // Stores selected bounds { startS, endS } for the interpolation tool
  dragStartRatio = null,      // Starts click-and-drag selection on canvas
  dragCurrentRatio = null;
const SLIDER_MAX_POINTS = 600;
// Poslednjih RECENT_SECONDS sekundi uvek ostaju na punoj rezoluciji (RECENT_BUDGET max tacaka);
// stariji deo se downsampluje uniformno u OLD_BUDGET tacaka.
// Na taj nacin brze promene u bliskoj proslosti ostaju vidljive na grafikonu,
// dok se stara istorija kompresuje da stedri na prostoru.
const RECENT_SECONDS = 7200;  // 2 sata punog detalja
const RECENT_BUDGET  = 400;   // max tacaka za novi region
const OLD_BUDGET     = 200;   // max tacaka za stari region

function rebuildSliderData() {
  const src = allChartData;
  if (!src.length) { sliderData = src; return; }

  // Uvek ukljuci poslednju tacku (najskorije ocitavanje)
  const newestSec = src[src.length - 1].s;
  const cutoff    = newestSec - RECENT_SECONDS;

  // Nadji granicu (splitIdx) izmedju starog i novog dela
  let splitIdx = 0;
  for (let i = src.length - 1; i >= 0; i--) {
    if (src[i].s <= cutoff) { splitIdx = i + 1; break; }
  }

  const oldSrc    = src.slice(0, splitIdx);
  const recentSrc = src.slice(splitIdx);
  const out = [];

  // Stari deo — uniformni downsampling
  if (oldSrc.length > 0) {
    const stride = oldSrc.length > OLD_BUDGET ? Math.ceil(oldSrc.length / OLD_BUDGET) : 1;
    for (let i = 0; i < oldSrc.length; i += stride) out.push(oldSrc[i]);
  }

  // Novi deo — pun detalj ili blago komprimovan ako ima previse tacaka
  if (recentSrc.length > 0) {
    if (recentSrc.length <= RECENT_BUDGET) {
      for (const p of recentSrc) out.push(p);
    } else {
      const stride = Math.ceil(recentSrc.length / RECENT_BUDGET);
      for (let i = 0; i < recentSrc.length; i += stride) out.push(recentSrc[i]);
      const last = recentSrc[recentSrc.length - 1];
      if (out[out.length - 1] !== last) out.push(last);
    }
  }

  sliderData = out;
}

// Detektuje ekstremna odstupanja (I2C glitch-eve, noise, nagle padove/skokove na nulu ili prekinute senzore)
// i glatko ih popunjava linearnom interpolacijom izmedju poslednjeg i sledeceg ispravnog očitavanja.
// Vraca ukupan broj ociscenih anomalija.
function interpolateOutliers(data) {
  if (data.length < 3) return 0;
  const isBad = new Array(data.length).fill(false);
  let cleanedCount = 0;
  
  // Prvi prolaz: ukloni grube out-of-range sensor failure vrednosti.
  for (let i = 0; i < data.length; i++) {
    const p = data[i];
    if (p.t < -40 || p.t > 80 || p.h < 0 || p.h > 100) {
      isBad[i] = true;
      cleanedCount++;
    }
  }

  // Vise-prolazni detektor: 3 uzastopna prolaza da bi se ulovili i susedni/vezani spajkovi.
  // Koristi najblize validne levo/desno tacke, ali samo ako su vremenski blizu — veliki razmak
  // (npr. period dok telefon nije bio povezan preko BLE) znaci da je promena verovatno stvarna,
  // ne senzorski glič, pa se preskace da se ne bi lazno "izravnala" prava izmerena promena.
  const MAX_NEIGHBOR_GAP_SEC = 600;
  for (let pass = 0; pass < 3; pass++) {
    for (let i = 1; i < data.length - 1; i++) {
      if (isBad[i]) continue;
      
      let leftIdx = i - 1;
      while (leftIdx >= 0 && isBad[leftIdx]) leftIdx--;
      let rightIdx = i + 1;
      while (rightIdx < data.length && isBad[rightIdx]) rightIdx++;
      
      if (leftIdx >= 0 && rightIdx < data.length) {
        const p = data[i];
        const prev = data[leftIdx];
        const next = data[rightIdx];
        if (p.s - prev.s > MAX_NEIGHBOR_GAP_SEC || next.s - p.s > MAX_NEIGHBOR_GAP_SEC) continue;
        
        const diffT1 = p.t - prev.t;
        const diffT2 = p.t - next.t;
        // Lokalne ostre t spikes
        if (diffT1 * diffT2 > 0 && Math.abs(diffT1) > 1.5 && Math.abs(diffT2) > 1.5) {
          isBad[i] = true;
          cleanedCount++;
          continue;
        }
        
        const diffH1 = p.h - prev.h;
        const diffH2 = p.h - next.h;
        // Lokalne ostre h spikes
        if (diffH1 * diffH2 > 0 && Math.abs(diffH1) > 5 && Math.abs(diffH2) > 5) {
          isBad[i] = true;
          cleanedCount++;
          continue;
        }
      }
    }
  }

  let firstValidIdx = -1;
  for (let i = 0; i < data.length; i++) {
    if (!isBad[i]) {
      firstValidIdx = i;
      break;
    }
  }
  if (firstValidIdx === -1) return cleanedCount;

  for (let i = 0; i < firstValidIdx; i++) {
    const valid = data[firstValidIdx];
    data[i].t = valid.t;
    data[i].h = valid.h;
    const d = derived(valid.t, valid.h);
    data[i].r = d.r;
    data[i].a = d.a;
  }

  let lastValidIdx = firstValidIdx;
  for (let i = firstValidIdx + 1; i < data.length; i++) {
    if (isBad[i]) {
      let nextValidIdx = -1;
      for (let j = i + 1; j < data.length; j++) {
        if (!isBad[j]) {
          nextValidIdx = j;
          break;
        }
      }
      if (nextValidIdx !== -1) {
        const t0 = data[lastValidIdx].s;
        const t1 = data[nextValidIdx].s;
        const t = data[i].s;
        const fraction = (t0 === t1) ? 0 : (t - t0) / (t1 - t0);
        const valT = data[lastValidIdx].t + fraction * (data[nextValidIdx].t - data[lastValidIdx].t);
        const valH = data[lastValidIdx].h + fraction * (data[nextValidIdx].h - data[lastValidIdx].h);
        data[i].t = valT;
        data[i].h = valH;
        const d = derived(valT, valH);
        data[i].r = d.r;
        data[i].a = d.a;
      } else {
        const valid = data[lastValidIdx];
        data[i].t = valid.t;
        data[i].h = valid.h;
        const d = derived(valid.t, valid.h);
        data[i].r = d.r;
        data[i].a = d.a;
      }
    } else {
      lastValidIdx = i;
    }
  }
  return cleanedCount;
}
const chartDefs = [
  ["temperature", "t", "#c76432", 0.6, "C"],
  ["humidity", "h", "#2878b8", 1.5, "%"],
  ["realFeel", "r", "#8b4aa5", 0.6, "C"],
  ["absoluteHumidity", "a", "#087e6a", 0.6, "g/m3"],
];
function clamp(v, min, max) {
  return Math.min(max, Math.max(min, v));
}
function formatMoment(s, full = false) {
  if (s > 1700000000) {
    const d = new Date(s * 1000);
    return full
      ? d.toLocaleString()
      : d.toLocaleTimeString([], {
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit",
        });
  }
  const h = Math.floor(s / 3600),
    m = Math.floor((s % 3600) / 60),
    q = s % 60;
  return "T+" + [h, m, q].map((n) => String(n).padStart(2, "0")).join(":");
}
function formatDate(s) {
  return s > 1700000000
    ? new Date(s * 1000).toLocaleDateString()
    : "od ukljucenja";
}
function formatDuration(seconds) {
  seconds = Math.max(0, Math.round(seconds));
  const days = Math.floor(seconds / 86400),
    hours = Math.floor((seconds % 86400) / 3600),
    minutes = Math.floor((seconds % 3600) / 60);
  const parts = [];
  if (days) parts.push(days + "d");
  if (hours) parts.push(hours + "h");
  if (minutes || parts.length === 0) parts.push(minutes + "min");
  return "Opseg: " + parts.slice(0, 2).join(" ");
}
function formatDurationNoPrefix(seconds) {
  seconds = Math.max(0, Math.round(seconds));
  const days = Math.floor(seconds / 86400),
    hours = Math.floor((seconds % 86400) / 3600),
    minutes = Math.floor((seconds % 3600) / 60);
  const parts = [];
  if (days) parts.push(days + "d");
  if (hours) parts.push(hours + "h");
  if (minutes || parts.length === 0) parts.push(minutes + "min");
  return parts.slice(0, 2).join(" ");
}

function nearestIndex(data, t) {
  if (!data.length) return 0;
  let best = 0,
    dist = Infinity;
  for (let i = 0; i < data.length; i++) {
    const d = Math.abs(data[i].s - t);
    if (d < dist) {
      dist = d;
      best = i;
    }
  }
  return best;
}
function drawChart(name, data, key, color, minSpan) {
  const c = $(name + "Chart"),
    d = devicePixelRatio || 1,
    w = c.clientWidth,
    h = c.clientHeight;
  c.width = w * d;
  c.height = h * d;
  const x = c.getContext("2d");
  x.setTransform(1, 0, 0, 1, 0, 0);
  x.clearRect(0, 0, c.width, c.height);
  x.scale(d, d);
  const v = data.map((p) => p[key]),
    deltaEl = $(name + "Delta");
  if (v.length < 2) {
    $(name + "Max").textContent =
      $(name + "Min").textContent =
      deltaEl.textContent =
        "--";
    return;
  }
  const rawLo = Math.min(...v),
    rawHi = Math.max(...v),
    change = v[v.length - 1] - v[0],
    shownChange = Math.abs(change) < 0.05 ? 0 : change;
  let lo = rawLo,
    hi = rawHi,
    mid = (lo + hi) / 2;
  if (hi - lo < minSpan) {
    lo = mid - minSpan / 2;
    hi = mid + minSpan / 2;
  }
  $(name + "Max").textContent = rawHi.toFixed(1);
  $(name + "Min").textContent = rawLo.toFixed(1);
  deltaEl.textContent = (shownChange > 0 ? "+" : "") + shownChange.toFixed(1);
  deltaEl.className =
    "rangeLabel rangeDelta " +
    (shownChange > 0 ? "up" : shownChange < 0 ? "down" : "");
  const first = data[0].s,
    last = data[data.length - 1].s,
    span = Math.max(1, last - first),
    pts = data.map((p, i) => ({
      x: ((p.s - first) * w) / span,
      y: h - 5 - ((v[i] - lo) / (hi - lo)) * (h - 10),
    }));
  x.strokeStyle = "#deddd4";
  x.beginPath();
  x.moveTo(0, h - 1);
  x.lineTo(w, h - 1);
  x.stroke();

  // Draw chart paths segment by segment, leaving dashed faded lines across gaps where dt > 120s or 5x the average spacing
  const totalDurationSec = data[data.length - 1].s - data[0].s;
  const avgSpacingSec = totalDurationSec / data.length;
  const gapThresholdSec = Math.max(120, avgSpacingSec * 5);

  x.lineWidth = 2;
  x.lineJoin = "round";
  x.lineCap = "round";

  let i = 0;
  while (i < pts.length) {
    let startSegment = i;
    // Find where the continuous segment ends (or where a gap occurs)
    while (i < pts.length - 1 && (data[i + 1].s - data[i].s) <= gapThresholdSec) {
      i++;
    }
    let endSegment = i;

    // Draw continuous segment representing valid, contiguous readings
    if (endSegment > startSegment) {
      x.strokeStyle = color;
      x.setLineDash([]);
      x.beginPath();
      x.moveTo(pts[startSegment].x, pts[startSegment].y);
      for (let j = startSegment + 1; j < endSegment; j++) {
        const mx = (pts[j].x + pts[j + 1].x) / 2,
          my = (pts[j].y + pts[j + 1].y) / 2;
        x.quadraticCurveTo(pts[j].x, pts[j].y, mx, my);
      }
      x.lineTo(pts[endSegment].x, pts[endSegment].y);
      x.stroke();
    } else {
      // Draw solitary point if any
      x.fillStyle = color;
      x.beginPath();
      x.arc(pts[startSegment].x, pts[startSegment].y, 1.5, 0, Math.PI * 2);
      x.fill();
    }

    // If there is a next segment, draw a dashed prigušena (faded) line across the sensor gap/disconnection
    if (i < pts.length - 1) {
      x.strokeStyle = "#b0ae9f";
      x.setLineDash([4, 4]);
      x.beginPath();
      x.moveTo(pts[i].x, pts[i].y);
      x.lineTo(pts[i + 1].x, pts[i + 1].y);
      x.stroke();
    }
    i++;
  }
  x.setLineDash([]); // Reset line dash for cursor drawing

  // Render a visual semi-transparent selection overlay if click-and-drag-zooming is in progress.
  if (dragStartRatio !== null && dragCurrentRatio !== null) {
    const leftX = Math.min(dragStartRatio, dragCurrentRatio) * w;
    const rightX = Math.max(dragStartRatio, dragCurrentRatio) * w;
    x.fillStyle = "rgba(8, 126, 106, 0.15)";
    x.fillRect(leftX, 0, rightX - leftX, h);
    x.strokeStyle = "rgba(8, 126, 106, 0.4)";
    x.lineWidth = 1;
    x.strokeRect(leftX, 0, rightX - leftX, h);
  }

  if (cursorIndex >= 0 && cursorIndex < pts.length) {
    const p = pts[cursorIndex];
    const metricMeta = {
      t: { label: "T", unit: "C" },
      h: { label: "RH", unit: "%" },
      r: { label: "RF", unit: "C" },
      a: { label: "AH", unit: "g/m3" },
    };
    const meta = metricMeta[key] || { label: key.toUpperCase(), unit: "" };
    const valueText = `${Number(data[cursorIndex][key]).toFixed(2)} ${meta.unit}`;
    x.save();
    x.strokeStyle = "#17211c";
    x.lineWidth = 1;
    x.setLineDash([4, 3]);
    x.beginPath();
    x.moveTo(p.x, 0);
    x.lineTo(p.x, h);
    x.stroke();
    x.restore();
    x.fillStyle = color;
    x.beginPath();
    x.arc(p.x, p.y, 4, 0, Math.PI * 2);
    x.fill();
    // Fixed corner position and large font so the value is legible and never jumps as the cursor moves.
    x.font = "700 20px Arial";
    x.textAlign = "right";
    x.textBaseline = "top";
    const fixedX = w - 6,
      fixedY = 4;
    x.lineWidth = 3;
    x.strokeStyle = "#f7f4ee";
    x.strokeText(valueText, fixedX, fixedY);
    x.fillStyle = color;
    x.fillText(valueText, fixedX, fixedY);
    x.textAlign = "left";
    x.textBaseline = "alphabetic";
  }
}

// Render only time labels under each chart's X axis (start, span, end)
function renderChartDividers(startS, endS) {
  const html = `<span>${formatMoment(startS)}</span>` +
               `<span class="dividerCenter">${formatDurationNoPrefix(endS - startS)}</span>` +
               `<span>${formatMoment(endS)}</span>`;
  chartDefs.forEach((c) => {
    const el = $(c[0] + "Times");
    if (el) el.innerHTML = html;
  });
}

function renderCharts() {
  if (chartData.length && cursorIndex >= chartData.length)
    cursorIndex = chartData.length - 1;
  chartDefs.forEach((c) => drawChart(c[0], chartData, c[1], c[2], c[3]));
  const cursorTimeEl = $("cursorTime");
  const dateEl = $("recordedDate");
  if (cursorIndex >= 0 && chartData[cursorIndex]) {
    cursorTimeEl.textContent = formatMoment(chartData[cursorIndex].s, true);
    cursorTimeEl.classList.remove("hidden");
    if (dateEl) dateEl.classList.add("hidden");
  } else {
    cursorTimeEl.classList.add("hidden");
    if (dateEl) dateEl.classList.remove("hidden");
  }
  
  // Ako je rucno aktivirana selekcija, osiguraj da taster za interpolaciju bude vidljiv
  if (activeSelectedRange) {
    $("interpolateBtn").classList.remove("hidden");
  } else {
    $("interpolateBtn").classList.add("hidden");
  }
}
function applyTimeRange(isUserInput = false) {
  const n = sliderData.length;
  if (n < 2) {
    chartData = sliderData;
    renderCharts();
    return;
  }
  if (isUserInput) {
    liveRangeFollow = false;
  }
  const minGap = Math.max(1, Math.round(n * 0.02));
  sliderStart = clamp(sliderStart, 0, n - 1);
  sliderEnd = clamp(sliderEnd, sliderStart + minGap, n - 1);

  rangeWindowSize = Math.max(minGap, sliderEnd - sliderStart);
  const max = n - 1,
    start = sliderData[sliderStart].s,
    end = sliderData[sliderEnd].s,
    d1 = formatDate(start),
    d2 = formatDate(end);
    
  // Update visually on track
  $("thumbStart").style.left = (sliderStart / max) * 100 + "%";
  
  const endThumb = $("thumbEnd");
  endThumb.style.left = (sliderEnd / max) * 100 + "%";
  if (liveRangeFollow && sliderEnd >= max - 1) {
    endThumb.classList.add("liveBlink");
  } else {
    endThumb.classList.remove("liveBlink");
  }
  
  timelineFill.style.left = (sliderStart / max) * 100 + "%";
  timelineFill.style.width = ((sliderEnd - sliderStart) / max) * 100 + "%";
  
  renderChartDividers(start, end);
  
  recordedFrom.textContent = formatMoment(start);
  recordedTo.textContent = formatMoment(end);
  recordedDate.textContent = d1 === d2 ? d1 : d1 + " / " + d2;
  chartData = sliderData.slice(sliderStart, sliderEnd + 1);
  if (cursorIndex >= chartData.length) cursorIndex = chartData.length - 1;
  renderCharts();
}
function resetTimeRange() {
  liveRangeFollow = true;
  sliderStart = 0;
  sliderEnd = Math.max(1, sliderData.length - 1);
  applyTimeRange();
}
function bindRangeThumbZIndex() {
  const track = $("timelineTrack");
  const thumbStartEl = $("thumbStart");
  const thumbEndEl = $("thumbEnd");
  const fillEl = $("timelineFill");

  if (!track || !thumbStartEl || !thumbEndEl || !fillEl) return;

  let activeDrag = null; // "start", "end", or "fill"
  let startX = 0;
  let startSliderStart = 0;
  let startSliderEnd = 0;
  let thumbEndPressTimer = null;

  const getIndexFromX = (clientX) => {
    const rect = track.getBoundingClientRect();
    const pct = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
    return Math.round(pct * (sliderData.length - 1));
  };

  const onPointerDown = (e) => {
    e.preventDefault();
    track.setPointerCapture(e.pointerId);

    const rect = track.getBoundingClientRect();
    const touchX = e.clientX;
    const currentIdx = getIndexFromX(touchX);

    // Detektuj sta je pritisnuto
    if (e.target === thumbStartEl) {
      activeDrag = "start";
    } else if (e.target === thumbEndEl) {
      activeDrag = "end";
      // Lock/Unlock dugim drzanjem (800ms)
      if (thumbEndPressTimer) clearTimeout(thumbEndPressTimer);
      thumbEndPressTimer = setTimeout(() => {
        liveRangeFollow = !liveRangeFollow;
        const msg = liveRangeFollow 
          ? "Praćenje u realnom vremenu UKLJUČENO (slajder blinka i zaključan je)."
          : "Praćenje u realnom vremenu ISKLJUČENO (slajder otključan za ručni pregled).";
        alert(msg);
        applyTimeRange(true);
        if (thumbEndPressTimer) {
          clearTimeout(thumbEndPressTimer);
          thumbEndPressTimer = null;
        }
      }, 800);
    } else if (e.target === fillEl) {
      activeDrag = "fill";
      startX = touchX;
      startSliderStart = sliderStart;
      startSliderEnd = sliderEnd;
    } else {
      // Pritisak na samu sinu
      const distToStart = Math.abs(currentIdx - sliderStart);
      const distToEnd = Math.abs(currentIdx - sliderEnd);
      if (distToStart < distToEnd) {
        activeDrag = "start";
        sliderStart = clamp(currentIdx, 0, sliderEnd - 1);
        applyTimeRange(true);
      } else {
        activeDrag = "end";
        sliderEnd = clamp(currentIdx, sliderStart + 1, sliderData.length - 1);
        applyTimeRange(true);
      }
    }
  };

  const onPointerMove = (e) => {
    if (activeDrag === "end" && thumbEndPressTimer) {
      // Ako pomera, ponisti longpress detekciju
      clearTimeout(thumbEndPressTimer);
      thumbEndPressTimer = null;
    }

    if (!activeDrag) return;
    const n = sliderData.length;
    if (n < 2) return;
    const minGap = Math.max(1, Math.round(n * 0.02));
    const currentIdx = getIndexFromX(e.clientX);

    if (activeDrag === "start") {
      sliderStart = clamp(currentIdx, 0, sliderEnd - minGap);
      applyTimeRange(true);
    } else if (activeDrag === "end") {
      // Ako je follow ukljucen i vuce se rucno, rucno iskljuci follow
      if (liveRangeFollow) {
        liveRangeFollow = false;
        thumbEndEl.classList.remove("liveBlink");
      }
      sliderEnd = clamp(currentIdx, sliderStart + minGap, n - 1);
      applyTimeRange(true);
    } else if (activeDrag === "fill") {
      const rect = track.getBoundingClientRect();
      const dxPct = (e.clientX - startX) / rect.width;
      const dxIdx = Math.round(dxPct * (n - 1));
      
      const width = startSliderEnd - startSliderStart;
      let newStart = startSliderStart + dxIdx;
      let newEnd = newStart + width;
      
      if (newStart < 0) {
        newStart = 0;
        newEnd = width;
      }
      if (newEnd > n - 1) {
        newEnd = n - 1;
        newStart = newEnd - width;
      }
      sliderStart = newStart;
      sliderEnd = newEnd;
      applyTimeRange(true);
    }
  };

  const onPointerUp = (e) => {
    if (thumbEndPressTimer) {
      clearTimeout(thumbEndPressTimer);
      thumbEndPressTimer = null;
    }
    if (!activeDrag) return;
    track.releasePointerCapture(e.pointerId);
    activeDrag = null;
  };

  thumbStartEl.addEventListener("pointerdown", onPointerDown);
  thumbEndEl.addEventListener("pointerdown", onPointerDown);
  fillEl.addEventListener("pointerdown", onPointerDown);
  track.addEventListener("pointerdown", onPointerDown);
  
  // Clean, modern global tracking on move/up so dragging never stutters, drops or lock outs outside the bounds.
  window.addEventListener("pointermove", onPointerMove);
  window.addEventListener("pointerup", onPointerUp);
  window.addEventListener("pointercancel", onPointerUp);
}

const HISTORY_STORAGE_KEY = "voicetoys_ble_history_v1";
const MAX_STORED_HISTORY_POINTS = 20000;
function loadStoredHistory() {
  try {
    const raw = localStorage.getItem(HISTORY_STORAGE_KEY);
    if (!raw) return;
    const stored = JSON.parse(raw);
    if (!Array.isArray(stored)) return;
    const nowSec = Math.round(Date.now() / 1000);
    // Filtriraj stara nevalidna uptime vremena i tacke koje su starim bagom upisane u daleku buducnost.
    allChartData = stored
      .filter((p) => p.s >= 1704067200 && p.s <= nowSec + 3600)
      .map((p) => {
        const d = derived(p.t, p.h);
        return { s: p.s, t: p.t, h: p.h, r: d.r, a: d.a };
      });
  } catch (e) {
    console.error("Neuspelo ucitavanje sacuvane istorije", e);
  }
}
function persistHistory() {
  try {
    const trimmed =
      allChartData.length > MAX_STORED_HISTORY_POINTS
        ? allChartData.slice(allChartData.length - MAX_STORED_HISTORY_POINTS)
        : allChartData;
    const compact = trimmed.map((p) => ({ s: p.s, t: p.t, h: p.h }));
    localStorage.setItem(HISTORY_STORAGE_KEY, JSON.stringify(compact));
  } catch (e) {
    console.error("Neuspelo cuvanje istorije", e);
  }
}
async function refreshCharts() {
  if (isNative) {
    if (deviceId) {
      // Send command to pull history over BLE
      requestHistory();
    }
    return;
  }

  // Ako korisnik aktivno vrsi rucnu interpolaciju, privremeno pauziraj
  // automatsko povlacenje istorije da ruter ne bi pregazio nase izmene.
  if (activeSelectedRange) return;

  try {
    const d = await fetch("/history?range=86400", { cache: "no-store" }).then((r) => r.json());
    if (!d.length) return;
    allChartData = d;
    interpolateOutliers(allChartData);
    updateChartRange();
  } catch (e) {}
}

function updateChartRange() {
  // Anchor on timestamps (not raw indices), since sliderData's resampling stride can change as allChartData grows.
  const hadSelection = rangeReady && sliderData.length > 1;
  const selStartTime = hadSelection ? sliderData[sliderStart].s : null;
  const selEndTime = hadSelection ? sliderData[sliderEnd].s : null;
  const wasFollowing = liveRangeFollow;
  const windowSeconds = hadSelection ? Math.max(1, selEndTime - selStartTime) : 0;

  rebuildSliderData();
  const max = sliderData.length - 1;

  if (!rangeReady) {
    sliderStart = 0;
    sliderEnd = max;
    rangeReady = true;
    liveRangeFollow = true;
  } else if (wasFollowing) {
    // Keep following the newest data, preserving the same time-width window.
    const newestTime = sliderData[max].s;
    sliderStart = nearestIndex(sliderData, newestTime - windowSeconds);
    sliderEnd = max;
  } else {
    // Re-anchor the user's chosen time window onto the freshly resampled series; never snap back to "now".
    sliderStart = nearestIndex(sliderData, selStartTime);
    sliderEnd = nearestIndex(sliderData, selEndTime);
  }
  applyTimeRange();
}

function setCursorFromEvent(e) {
  if (chartData.length < 2) return;
  const r = e.currentTarget.getBoundingClientRect(),
    ratio = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
  cursorIndex = Math.round(ratio * (chartData.length - 1));
  renderCharts();
}
function applyPinchZoom(scale, startRange, pivotRatio) {
  const n = sliderData.length;
  if (n < 2) return;
  const minGap = Math.max(1, Math.round(n * 0.02));
  
  // Izracunaj tacnu apsolutnu vremensku tacku (sekunde) pod prstima (pivot)
  const duration = startRange.endSec - startRange.startSec;
  const pivotSec = startRange.startSec + pivotRatio * duration;
  
  // Izracunaj novu sirinu opsega i raspodeli je srazmerno pivot tacki
  const newDuration = clamp(Math.round(duration * scale), 120, 86400 * 7);
  const newStartSec = pivotSec - pivotRatio * newDuration;
  const newEndSec = newStartSec + newDuration;
  
  sliderStart = nearestIndex(sliderData, newStartSec);
  sliderEnd = nearestIndex(sliderData, newEndSec);
  
  if (sliderEnd - sliderStart < minGap) {
    sliderEnd = Math.min(n - 1, sliderStart + minGap);
  }
  applyTimeRange(true);
}

// One finger (or a hovering mouse) drives the vertical inspection guide; a mouse drag selects a
// range to zoom into and two fingers pinch-zoom, all directly on the chart itself.
//
// PINCH ZOOM — two-anchor pristup:
// Svaki prst "drzi" svoju vremensku tacku zakacenu za sebe tokom celog gesta.
// Levi prst vuci levu granicu, desni prst vuci desnu granicu — nema zajednicke pivot tacke.
// Matematika: znamo koje vreme je bilo pod levim (tL) i desnim (tR) prstom na pocetku pincha.
// Na svakom move-u, ako je levi prst na ratio rL a desni na rR:
//   newDuration = (tR - tL) / (rR - rL)
//   newStartSec = tL - rL * newDuration
function bindChartCursors() {
  const pointers = new Map();
  // Cuva vremensku tacku (sekunde) koja je bila pod levim/desnim prstom pri pocetku pincha.
  let pinchLeftStartSec  = null,
      pinchRightStartSec = null,
      pinchTarget        = null;

  const getXRatio = (clientX, target) => {
    const rect = target.getBoundingClientRect();
    return clamp((clientX - rect.left) / rect.width, 0, 1);
  };

  const beginPinch = (target) => {
    dragStartRatio = null;
    dragCurrentRatio = null;
    cursorIndex = -1;
    pinchTarget = target;
    const pts = [...pointers.values()].slice(0, 2);
    // Sortiraj po X — pts[0] je levi prst, pts[1] je desni
    const [lp, rp] = pts[0].x <= pts[1].x ? [pts[0], pts[1]] : [pts[1], pts[0]];
    const startSec   = sliderData[sliderStart].s;
    const duration   = sliderData[sliderEnd].s - startSec;
    const leftRatio  = getXRatio(lp.x, target);
    const rightRatio = getXRatio(rp.x, target);
    // Spremi tacne vremenske tacke pod svakim prstom
    pinchLeftStartSec  = startSec + leftRatio  * duration;
    pinchRightStartSec = startSec + rightRatio * duration;
  };

  const endPinch = () => {
    pinchLeftStartSec  = null;
    pinchRightStartSec = null;
    pinchTarget        = null;
  };

  // Applies the horizontal selection made on a chart as the new zoom range.
  const commitSelection = () => {
    if (dragStartRatio === null || dragCurrentRatio === null) return;
    const idxStart = sliderStart;
    const originalWidth = sliderEnd - sliderStart;
    const r0 = Math.min(dragStartRatio, dragCurrentRatio);
    const r1 = Math.max(dragStartRatio, dragCurrentRatio);

    if (r1 - r0 > 0.01) {
      const selStart = idxStart + Math.round(r0 * originalWidth);
      const selEnd = idxStart + Math.round(r1 * originalWidth);
      sliderStart = clamp(selStart, 0, sliderData.length - 2);
      sliderEnd = clamp(selEnd, sliderStart + 1, sliderData.length - 1);
      activeSelectedRange = {
        startS: sliderData[sliderStart].s,
        endS: sliderData[sliderEnd].s,
      };
      applyTimeRange(true);
    }
    dragStartRatio = null;
    dragCurrentRatio = null;
    renderCharts();
  };

  const onDown = (e) => {
    e.currentTarget.setPointerCapture(e.pointerId);
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });

    if (pointers.size >= 2 && sliderData.length > 1) {
      beginPinch(e.currentTarget);
      renderCharts();
      return;
    }

    if (e.pointerType === "mouse") {
      // Mouse drag draws a selection to zoom into; plain hovering keeps the inspection cursor.
      dragStartRatio = getXRatio(e.clientX, e.currentTarget);
      dragCurrentRatio = dragStartRatio;
      cursorIndex = -1;
      renderCharts();
    } else {
      setCursorFromEvent(e);
    }
  };

  const onMove = (e) => {
    if (pointers.has(e.pointerId)) {
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    }

    if (pointers.size >= 2 && pinchLeftStartSec !== null && pinchRightStartSec !== null && pinchTarget) {
      const pts = [...pointers.values()].slice(0, 2);
      // Sortiraj po X da bi levi/desni prst odgovarao levoj/desnoj vremenskoj tacki
      const [lp, rp] = pts[0].x <= pts[1].x ? [pts[0], pts[1]] : [pts[1], pts[0]];
      const lRatio = getXRatio(lp.x, pinchTarget);
      const rRatio = getXRatio(rp.x, pinchTarget);
      const spread = rRatio - lRatio;

      // Minimlani spread da se izbegne deljenje nulom / preterano zumiranje
      if (spread < 0.02) return;

      // Izracunaj novi vremenski opseg tako da levi prst ostaje na pinchLeftStartSec
      // a desni prst ostaje na pinchRightStartSec
      const rawDuration  = (pinchRightStartSec - pinchLeftStartSec) / spread;
      const newDuration  = clamp(Math.round(rawDuration), 120, 86400 * 7);
      const newStartSec  = pinchLeftStartSec - lRatio * newDuration;
      const newEndSec    = newStartSec + newDuration;

      const n      = sliderData.length;
      const minGap = Math.max(1, Math.round(n * 0.02));
      sliderStart  = nearestIndex(sliderData, newStartSec);
      sliderEnd    = nearestIndex(sliderData, newEndSec);
      if (sliderEnd - sliderStart < minGap) sliderEnd = Math.min(n - 1, sliderStart + minGap);
      applyTimeRange(true);
      return;
    }

    if (dragStartRatio !== null) {
      dragCurrentRatio = getXRatio(e.clientX, e.currentTarget);
      renderCharts();
      return;
    }

    if (e.pointerType === "mouse" && e.buttons === 0) {
      setCursorFromEvent(e);
      return;
    }

    if (e.pointerType === "touch" && pointers.size === 1) {
      setCursorFromEvent(e); // 1-finger guides the vertical line smoothly
    }
  };

  const onUp = (e) => {
    pointers.delete(e.pointerId);
    if (pointers.size < 2) endPinch();

    if (dragStartRatio !== null) {
      commitSelection();
      return;
    }

    if (e.pointerType === "mouse" || pointers.size === 0) {
      cursorIndex = -1;
      renderCharts();
    }
  };

  chartDefs.forEach((c) => {
    const el = $(c[0] + "Chart");
    el.addEventListener("pointerdown", onDown);
    el.addEventListener("pointermove", onMove);
    el.addEventListener("pointerup", onUp);
    el.addEventListener("pointercancel", onUp);
    el.addEventListener("pointerleave", (e) => {
      if (e.pointerType === "mouse" && dragStartRatio === null) {
        cursorIndex = -1;
        renderCharts();
      }
    });
  });
}


function esc(v) {
  const d = document.createElement("div");
  d.textContent = v || "";
  return d.innerHTML;
}
function escAttr(v) {
  return esc(v).replace(/'/g, "&#39;");
}
function derived(t, h) {
  const e = (h / 100) * 6.105 * Math.exp((17.27 * t) / (237.7 + t));
  const r = t + 0.33 * e - 4;
  const es = 6.112 * Math.exp((17.67 * t) / (t + 243.5));
  const a = (216.7 * ((es * h) / 100)) / (273.15 + t);
  return { r, a };
}
let deleteTimer = null;
function startDelete(id) {
  cancelDelete();
  deleteTimer = setTimeout(() => deleteSnapshot(id), 900);
}
function cancelDelete() {
  if (deleteTimer) {
    clearTimeout(deleteTimer);
    deleteTimer = null;
  }
}
async function deleteSnapshot(id) {
  cancelDelete();
  if (!confirm("Obrisati ovaj snapshot?")) return;
  const p = new URLSearchParams({ id });
  const r = await fetch("/snapshot/delete", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: p,
  });
  message.textContent = r.ok ? "Snapshot je obrisan." : "Brisanje nije uspelo.";
  await loadSnapshots();
}
async function loadSnapshots() {
  const list = await fetch("/snapshots").then((r) => r.json());
  snapshots.innerHTML = list
    .map((s) => {
      const d = derived(s.temperature, s.humidity);
      return `<tr class='snapshotRow' onpointerdown='startDelete(${s.id})' onpointerup='cancelDelete()' onpointercancel='cancelDelete()' onpointerleave='cancelDelete()'><td>${esc(new Date(s.capturedAt).toLocaleString())}</td><td>${s.temperature.toFixed(2)} &deg;C</td><td>${s.humidity.toFixed(2)} %</td><td>${d.r.toFixed(2)} &deg;C</td><td>${d.a.toFixed(2)} g/m&sup3;</td><td><input maxlength='95' value='${escAttr(s.comment)}' onpointerdown='event.stopPropagation()' onchange='saveComment(${s.id},this.value)'></td></tr>`;
    })
    .join("");
}
let pendingSnapshot = null;
async function captureSnapshot() {
  const btn = $("snapshotButton");
  btn.disabled = true;
  message.textContent = "Pravim snapshot...";
  await refresh();
  if (!live || !live.ahtReadingValid) {
    message.textContent = "Nema ispravnog ocitavanja.";
    btn.disabled = false;
    return;
  }
  pendingSnapshot = {
    capturedAt: new Date().toISOString(),
    temperature: live.lastTemperature,
    humidity: live.lastHumidity,
    realFeel: live.lastRealFeel,
    absoluteHumidity: live.lastAbsoluteHumidity,
  };
  temperatureSnapshot.textContent =
    "snapshot: " + pendingSnapshot.temperature.toFixed(2) + " C";
  humiditySnapshot.textContent =
    "snapshot: " + pendingSnapshot.humidity.toFixed(2) + " %";
  realFeelSnapshot.textContent =
    "snapshot: " + pendingSnapshot.realFeel.toFixed(2) + " C";
  absoluteHumiditySnapshot.textContent =
    "snapshot: " + pendingSnapshot.absoluteHumidity.toFixed(2) + " g/m3";
  $("saveSnapshotButton").classList.remove("hidden");
  btn.disabled = false;
  message.textContent = "Snapshot je pripremljen. Klikni Sacuvaj snapshot.";
}
async function savePendingSnapshot() {
  if (!pendingSnapshot) return;
  const saveBtn = $("saveSnapshotButton");
  saveBtn.disabled = true;
  const s = pendingSnapshot,
    p = new URLSearchParams({
      capturedAt: s.capturedAt,
      t: s.temperature,
      h: s.humidity,
    });
  const r = await fetch("/snapshot", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: p,
  });
  message.textContent = r.ok
    ? "Snapshot je trajno sacuvan."
    : "Cuvanje nije uspelo.";
  saveBtn.disabled = false;
  if (r.ok) {
    pendingSnapshot = null;
    saveBtn.classList.add("hidden");
    temperatureSnapshot.textContent = "";
    humiditySnapshot.textContent = "";
    realFeelSnapshot.textContent = "";
    absoluteHumiditySnapshot.textContent = "";
    await loadSnapshots();
  }
}
async function saveComment(id, comment) {
  const p = new URLSearchParams({ id, comment });
  await fetch("/snapshot/comment", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: p,
  });
  message.textContent = "Komentar je sacuvan.";
}
let knownNetworkNames = [];
async function loadNetwork() {
  try {
    const n = await fetch("/network/status", { cache: "no-store" }).then((r) =>
      r.json(),
    );
    networkState.innerHTML = n.connected
      ? `Povezano na <b>${esc(n.ssid)}</b><br>IP: <span class='address'>${esc(n.ip)}</span>`
      : `Fallback AP je aktivan: <b>${esc(n.apSsid)}</b><br>IP: <span class='address'>${esc(n.ip)}</span>`;
    knownNetworkNames = n.known;
    knownNetworks.innerHTML = n.known.length
      ? n.known
          .map(
            (s, i) =>
              `<div class='networkItem'><span>${esc(s)}</span><button class='danger' onclick='deleteNetworkByIndex(${i})'>Obrisi</button></div>`,
          )
          .join("")
      : `<span class='note'>Nema zapamcenih mreza.</span>`;
  } catch (e) {
    networkState.textContent = "Status trenutno nije dostupan.";
  }
}
async function scanNetworks() {
  networkMessage.textContent = "Pretrazujem...";
  try {
    const list = await fetch("/network/scan", { cache: "no-store" }).then((r) =>
        r.json(),
      ),
      seen = new Set();
    availableNetworks.innerHTML = "";
    list
      .filter((n) => !seen.has(n.ssid) && seen.add(n.ssid))
      .sort((a, b) => b.rssi - a.rssi)
      .forEach((n) => {
        const o = document.createElement("option");
        o.value = n.ssid;
        o.label =
          n.rssi + " dBm" + (n.secure ? " · zakljucana" : " · otvorena");
        availableNetworks.appendChild(o);
      });
    networkMessage.textContent = list.length
      ? "Izaberi mrezu ili upisi skriveni SSID."
      : "Nijedna mreza nije pronadjena; SSID mozes uneti rucno.";
  } catch (e) {
    networkMessage.textContent = "Pretraga nije uspela.";
  }
}
async function saveNetwork() {
  const ssid = networkSsid.value;
  if (!ssid) {
    networkMessage.textContent = "Prvo izaberi mrezu.";
    return;
  }
  networkMessage.textContent = "Cuvam mrezu...";
  const p = new URLSearchParams({ ssid, password: networkPassword.value });
  try {
    const r = await fetch("/network/save", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: p,
      }),
      text = await r.text();
    networkMessage.textContent = r.ok
      ? "Sacuvano. Telefon vrati na kucni Wi-Fi, pa otvori ws.local:8081"
      : text;
    if (r.ok) networkPassword.value = "";
  } catch (e) {
    networkMessage.textContent =
      "Veza je prekinuta radi povezivanja. Vrati telefon na kucni Wi-Fi i otvori ws.local:8081";
  }
}
function deleteNetworkByIndex(i) {
  if (i >= 0 && i < knownNetworkNames.length)
    deleteNetwork(knownNetworkNames[i]);
}
async function deleteNetwork(ssid) {
  if (!confirm("Obrisati mrezu " + ssid + "?")) return;
  const p = new URLSearchParams({ ssid });
  const r = await fetch("/network/delete", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: p,
  });
  networkMessage.textContent = await r.text();
  if (r.ok) await loadNetwork();
}
function health(el, ok, text) {
  el.className = "debugValue " + (ok ? "ok" : "bad");
  el.textContent = text;
}
async function loadDebug() {
  if (isNative) {
    health(
      debugSensor,
      !!live && live.ahtReadingValid,
      live && live.ahtReadingValid
        ? "Radi (BLE) \u00b7 " + live.lastTemperature.toFixed(2) + " C / " + live.lastHumidity.toFixed(2) + " %"
        : "Nema ocitavanja",
    );
    debugBattery.className = "debugValue";
    debugBattery.textContent = "Nije dostupno preko BLE";
    debugBq.textContent = "";
    health(debugBle, !!deviceId, deviceId ? "Povezan" : "Nije povezan");
    health(debugOta, false, "Dostupno samo preko WiFi/OTA rezima");
    health(debugWifi, false, "N/A (BLE mod)");
    wifiMessage.textContent = "Status Wi-Fi radija nije vidljiv preko BLE; prekidac samo salje komandu.";
    debugSystem.textContent = "N/A (BLE mod)";
    debugLogs.textContent = "Debug log je dostupan samo preko WiFi/OTA rezima.";
    return;
  }
  try {
    const [s, logs] = await Promise.all([
      fetch("/status", { cache: "no-store" }).then((r) => r.json()),
      fetch("/logs", { cache: "no-store" }).then((r) => r.text()),
    ]);
    health(
      debugSensor,
      s.ahtReadingValid,
      s.ahtReadingValid
        ? "Radi · " +
            s.lastTemperature.toFixed(2) +
            " C / " +
            s.lastHumidity.toFixed(2) +
            " %"
        : s.ahtPresent
          ? "Pronadjen, bez validnog merenja"
          : "Nije pronadjen",
    );
    health(
      debugBattery,
      s.bqPresent && s.bqReadHealthy,
      s.lastBatteryV > 0 ? s.lastBatteryV.toFixed(2) + " V" : "Nema merenja",
    );
    debugBq.textContent = s.bqPresent
      ? s.bqReadHealthy
        ? "BQ komunikacija je ispravna"
        : "BQ greska pri citanju"
      : "BQ nije pronadjen";
    health(
      debugBle,
      true,
      s.bleConnected ? "Klijent povezan" : "Aktivan · nema klijenta",
    );
    health(
      debugOta,
      s.webOtaActive,
      s.webOtaActive ? "Aktivan · port 8080" : "Nije aktivan",
    );
    health(
      debugWifi,
      s.wifiRssi < 0,
      s.wifiRssi < 0 ? s.wifiRssi + " dBm" : "Fallback AP",
    );
    wifiToggle.checked = s.wifiEnabled;
    debugSystem.textContent =
      Math.round(s.freeHeap / 1024) +
      " KB · " +
      Math.floor(s.uptimeSeconds / 60) +
      " min · core " +
      s.appCore;
    diagnosticLedToggle.checked = s.ledsEnabled;
    debugLogs.textContent = logs || "Log je prazan.";
  } catch (e) {
    debugMessage.textContent = "Debug podaci nisu dostupni.";
  }
}
async function setDiagnosticLeds(enabled) {
  debugMessage.textContent = "Cuvam...";
  const p = new URLSearchParams({ enabled: enabled ? "1" : "0" });
  const r = await fetch("/debug/leds", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: p,
  });
  debugMessage.textContent = r.ok
    ? enabled
      ? "RGB dijagnostika je ukljucena."
      : "RGB dijagnostika je iskljucena."
    : "Promena nije uspela.";
}
async function setWifiToggle(enabled) {
  wifiMessage.textContent = "Cuvam...";
  try {
    if (isNative) {
      await setWifiEnabled(enabled);
    } else {
      await fetch("/debug/wifi", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: new URLSearchParams({ enabled: enabled ? "1" : "0" }),
      });
    }
    wifiMessage.textContent = enabled ? "Wi-Fi je ukljucen." : "Wi-Fi je iskljucen.";
  } catch (e) {
    wifiMessage.textContent = "Promena nije uspela.";
  }
}
function triggerManualSmoothing() {
  const cleaned = interpolateOutliers(allChartData);
  rebuildSliderData();
  applyTimeRange();
  alert(`Grafikon je uspešno ispeglan! Očišćeno i interpolirano je ukupno ${cleaned} spajkova (anomalija).`);
}
async function pushInterpolationToStation(startS, endS) {
  try {
    if (isNative) {
      await interpolateHistory(startS, endS);
    } else {
      await fetch("/history/interpolate", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: new URLSearchParams({ start: String(startS), end: String(endS) }),
      });
    }
  } catch (e) {
    console.error("Interpolacija nije poslata stanici", e);
  }
}
function interpolateSelectedRange() {
  const sel = activeSelectedRange;
  if (!sel) return;
  if (!confirm(`Da li želiš da interpoliraš i poravnaš očitavanja u izabranom opsegu od ${formatMoment(sel.startS, true)} do ${formatMoment(sel.endS, true)}?`)) {
    return;
  }

  // Stanica je vlasnik podataka: ispravka se upisuje u njenu memoriju da bi je videli i app i web.
  pushInterpolationToStation(sel.startS, sel.endS);

  let leftIdx = -1;
  let rightIdx = -1;
  
  // Nadji najbize ispravne tacke levo i desno van opsega
  for (let i = 0; i < allChartData.length; i++) {
    if (allChartData[i].s < sel.startS) {
      leftIdx = i;
    }
  }
  for (let i = 0; i < allChartData.length; i++) {
    if (allChartData[i].s > sel.endS) {
      rightIdx = i;
      break;
    }
  }
  
  if (leftIdx !== -1 && rightIdx !== -1) {
    const p0 = allChartData[leftIdx];
    const p1 = allChartData[rightIdx];
    const span = p1.s - p0.s;
    
    // Linearna interpolacija svih tacaka u selekciji
    for (let i = leftIdx + 1; i < rightIdx; i++) {
      const p = allChartData[i];
      const frac = span === 0 ? 0 : (p.s - p0.s) / span;
      p.t = p0.t + frac * (p1.t - p0.t);
      p.h = p0.h + frac * (p1.h - p0.h);
      const d = derived(p.t, p.h);
      p.r = d.r;
      p.a = d.a;
    }
    
    persistHistory();
    rebuildSliderData();
    applyTimeRange();
    
    // Sakrij interpolaciju
    $("interpolateBtn").classList.add("hidden");
    activeSelectedRange = null;
    alert("Izabrani opseg je uspešno interpolisan i poravnat.");
  } else {
    alert("Nije moguće interpolirati jer nedostaju granične tačke.");
  }
}
bindChartCursors();
bindRangeThumbZIndex();
setInterval(refresh, 500);
if (!isNative) {
  setInterval(refreshCharts, 5000);
}

refresh();
if (!isNative) refreshCharts();
if (!isNative) loadSnapshots();

if (isNative) {
  // hide HTTP specific tabs
  $('snapTab').style.display = 'none';
  $('networkTab').style.display = 'none';

  // Reuse whatever was already synced before, so a reconnect only pulls newer points.
  loadStoredHistory();
  if (allChartData.length) {
    updateChartRange();
  }

  const bleActionsWrap = $('bleActionsWrap');
  const bleBtn = document.createElement('button');
  bleBtn.id = 'bleConnectBtn';
  bleBtn.textContent = 'Poveži se ponovo';
  bleActionsWrap.appendChild(bleBtn);

  let connecting = false;
  async function attemptConnect(manual) {
    if (connecting || deviceId) return;
    connecting = true;
    bleBtn.disabled = true;
    bleBtn.textContent = 'Povezujem...';
    sensorState.textContent = 'Trazim VoiceToysWS uredjaj u blizini...';
    let ok = false;
    try {
      ok = await connectBle();
    } catch (e) {
      ok = false;
    }
    connecting = false;
    bleBtn.disabled = false;
    bleBtn.textContent = 'Poveži se ponovo';
    if (ok) {
      sensorState.textContent = 'BLE povezan! Sinhronizujem vreme...';
      try {
        await sendTime(Math.round(Date.now() / 1000));
      } catch (e) {
        console.error("Greska pri sinhronizaciji vremena preko BLE", e);
      }
      sensorState.textContent = 'BLE povezan! Povlacim istoriju...';
      // Nova konekcija uvek sme da pokrene sync, cak i ako je prethodni prekinut usred prenosa.
      historySyncBusy = false;
      // Kratka pauza da se MTU pregovor zavrsi pre prvog zahteva za istoriju.
      setTimeout(requestHistorySync, 300);
    } else if (manual) {
      sensorState.textContent = 'Uredjaj nije pronadjen. Proveri Bluetooth i blizinu uredjaja.';
    }
  }
  bleBtn.onclick = () => attemptConnect(true);

  // Automatski trazi i povezuje uredjaj bez ikakvog klika; ponavlja dok ne uspe ili dok se ne diskonektuje.
  attemptConnect(false);
  setInterval(() => {
    if (!deviceId) attemptConnect(false);
  }, 4000);

  // Sync ide u pozadini samo kad zivi podaci prestanu da sticu (znaci da nesto nedostaje), ne na svakih par sekundi.
  let lastLiveUpdateAt = Date.now();
  setInterval(() => {
    if (deviceId && Date.now() - lastLiveUpdateAt > 5000) requestHistorySync();
  }, 5000);

  // Watchdog: ako je deviceId setovan ali live paketi nisu stigli duze od 10s,
  // BLE stack je tiho pao (bez disconnect callbacka). Prisilno resetuj konekciju
  // da auto-reconnect loop moze da ponovo pokuša.
  setInterval(async () => {
    if (deviceId && Date.now() - lastLiveUpdateAt > 10000) {
      console.warn('BLE watchdog: nema live podataka 10s, prisilno diskonekcija...');
      sensorState.textContent = 'Veza prekinuta. Tražim ponovo...';
      await forceDisconnect();
      // deviceId je sada null — sledeci obrtaj auto-reconnect intervala ce ga pokupiti
    }
  }, 5000);

  // Periodično ažuriraj sensorState dok je BLE aktivan, da ghost poruka
  // ne ostane zauvek na ekranu ako ocitavanja kasne ili stanu.
  setInterval(() => {
    if (!isNative) return;
    if (!deviceId) {
      // Nije povezan — attemptConnect ce ažurirati poruku sam;
      // samo osiguraj da nema zaostalih "aktivan" poruka.
      if (!connecting) sensorState.textContent = 'Tražim VoiceToysWS uređaj u blizini...';
    } else if (Date.now() - lastLiveUpdateAt > 3000) {
      sensorState.textContent = 'BLE povezan · čekam očitavanje senzora...';
    }
  }, 2000);

  let syncTotal = 0,
    syncReceived = 0,
    historySyncBusy = false;
  function updateSyncUI(active) {
    const wrap = $('bleSyncWrap'),
      fill = $('syncFill'),
      text = $('syncText'),
      gitStatusEl = $("gitStatus");
    wrap.classList.remove('hidden');
    if (active) {
      const pct = syncTotal > 0 ? Math.min(100, Math.round((syncReceived / syncTotal) * 100)) : 0;
      fill.classList.remove('done');
      fill.style.width = pct + '%';
      text.textContent =
        'Sinhronizacija u toku... ' + syncReceived + '/' + (syncTotal || '?') + ' tacaka (' + pct + '%)';
      if (gitStatusEl) {
        gitStatusEl.innerHTML = `<span style="color: var(--down);">🔄 Sinhronizacija baze u toku...</span>`;
      }
    } else {
      fill.classList.add('done');
      fill.style.width = '100%';
      text.textContent =
        'Sinhronizacija zavrsena: ' + syncReceived + ' tacaka, ' + new Date().toLocaleTimeString();
      if (gitStatusEl) {
        gitStatusEl.innerHTML = `<span style="color: var(--up);">✅ Lokalna baza usaglašena sa stanicom</span>`;
      }
    }
  }
  function requestHistorySync() {
    if (!deviceId || historySyncBusy) return;
    historySyncBusy = true;
    const since = allChartData.length ? allChartData[allChartData.length - 1].s : 0;
    syncTotal = 0;
    syncReceived = 0;
    updateSyncUI(true);
    requestHistory(since);
  }

// Preracunava trend rasta/pada na osnovu istorijskih promena zabelezenih u lokalnoj bazi
function calculateLocalTrends(nowSec, newT, newH, newR, newA) {
  if (allChartData.length < 5) return;
  const targetTime = nowSec - 60;
  const idx = nearestIndex(allChartData, targetTime);
  const prev = allChartData[idx];
  const elapsedMin = (nowSec - prev.s) / 60;
  if (elapsedMin > 0.1) {
    const rateT = (newT - prev.t) / elapsedMin;
    const rateH = (newH - prev.h) / elapsedMin;
    const rateR = (newR - prev.r) / elapsedMin;
    const rateA = (newA - prev.a) / elapsedMin;
    setTrend("temperature", rateT, 18, "C");
    setTrend("humidity", rateH, 5, "%");
    setTrend("realFeel", rateR, 18, "C");
    setTrend("absoluteHumidity", rateA, 14, "g/m3");
  }
}

initBle((str) => {
    // onLiveUpdate (T:XX.XX,H:XX.XX,V:XX.XXV,S:XXXXXXX)
    let tMatch = str.match(/T:([0-9.-]+)/);
    let hMatch = str.match(/H:([0-9.-]+)/);
    let vMatch = str.match(/V:([0-9.-]+)/);
    let sMatch = str.match(/S:([0-9]+)/);
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

       // Grafikon raste odmah iz zivih ocitavanja sa tacnim apsolutnim vremenom sa senzora.
       lastLiveUpdateAt = Date.now();
       if (vMatch) updateBatteryUI(parseFloat(vMatch[1]));
       const s = sMatch ? parseInt(sMatch[1]) : Math.round(Date.now() / 1000);
       allChartData.push({ s, t, h, r: d.r, a: d.a });
       calculateLocalTrends(s, t, h, d.r, d.a);
       
       // Azuriraj "Git-like" status
       const gitStatusEl = $("gitStatus");
       if (gitStatusEl && allChartData.length > 1 && !historySyncBusy) {
         const lastDbS = allChartData[allChartData.length - 2].s; // pretposlednja tacna
         const diffSec = s - lastDbS;
         if (diffSec > 60) {
           gitStatusEl.innerHTML = `<span style="color: var(--down);">⚠️ Lokalna baza zaostaje za ${formatDurationNoPrefix(diffSec)}</span>`;
         } else {
           gitStatusEl.innerHTML = `<span style="color: var(--up);">✅ Lokalna baza usaglašena sa stanicom</span>`;
         }
       }
       
       updateChartRange();
    }
  }, (buf) => {
    // onHistoryUpdate (binary chunk)
    const dv = new DataView(buf);
    const numPoints = dv.byteLength / 8;
    for(let i = 0; i < numPoints; i++) {
       let s = dv.getUint32(i*8, true); // little endian
       let t100 = dv.getInt16(i*8 + 4, true);
       let h100 = dv.getInt16(i*8 + 6, true);

       if (s === 0xFFFFFFFE) {
         // Zaglavlje toka: t100 nosi ocekivani broj tacaka.
         syncTotal = t100;
         syncReceived = 0;
         updateSyncUI(true);
         continue;
       }

       if (s === 0xFFFFFFFF) {
         // EOF
         const nowSec = Math.round(Date.now() / 1000);
         // Ocisti iz niza sve sto nema validno Unix vreme ili je upisano u buducnost starim bagovima.
         allChartData = allChartData.filter((p) => p.s >= 1704067200 && p.s <= nowSec + 3600);
         allChartData.sort((a, b) => a.s - b.s);
         // Drop duplicate timestamps that can occur at the approximate delta-sync boundary.
         for (let i = allChartData.length - 1; i > 0; i--) {
           if (allChartData[i].s === allChartData[i - 1].s) allChartData.splice(i, 1);
         }
         if (allChartData.length > MAX_STORED_HISTORY_POINTS) {
           allChartData = allChartData.slice(allChartData.length - MAX_STORED_HISTORY_POINTS);
         }
         interpolateOutliers(allChartData);
         persistHistory();
         console.log("History EOF received. Total:", allChartData.length);
         updateSyncUI(false);
         historySyncBusy = false;
         updateChartRange();
         break;
       }
       
       let t = t100 / 100.0;
       let h = h100 / 100.0;
       let d = derived(t, h);
       allChartData.push({s, t, h, r: d.r, a: d.a});
       syncReceived++;
       updateSyncUI(true);
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

// Inline onclick="..." handlers need these on window since this file loads as an ES module.
Object.assign(window, {
  showTab,
  openUpdate,
  applyTimeRange,
  resetTimeRange,
  captureSnapshot,
  savePendingSnapshot,
  saveComment,
  scanNetworks,
  saveNetwork,
  deleteNetworkByIndex,
  setDiagnosticLeds,
  setWifiToggle,
  triggerManualSmoothing,
  interpolateSelectedRange,
  loadDebug,
  startDelete,
  cancelDelete,
});

