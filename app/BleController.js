import { Capacitor } from '@capacitor/core';
import { BleClient, numbersToDataView, dataViewToNumbers } from '@capacitor-community/bluetooth-le';

const SERVICE_UUID = '4fafc201-1fb5-459e-8fcc-c5c9c331914b';
const LIVE_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26a8';
const HISTORY_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26aa';

export let isNative = Capacitor.isNativePlatform();
export let deviceId = null;

let onLiveUpdate = null;
let onHistoryUpdate = null;

export async function initBle(liveCallback, historyCallback) {
  onLiveUpdate = liveCallback;
  onHistoryUpdate = historyCallback;
  
  try {
    await BleClient.initialize();
    console.log("BLE inicijalizovan");
  } catch(e) {
    console.error(e);
  }
}

export async function connectBle() {
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
    
    console.log("Povezan sa", deviceId);
    
    await BleClient.startNotifications(deviceId, SERVICE_UUID, LIVE_UUID, (value) => {
      let str = new TextDecoder().decode(value.buffer);
      if (onLiveUpdate) onLiveUpdate(str);
    });
    
    await BleClient.startNotifications(deviceId, SERVICE_UUID, HISTORY_UUID, (value) => {
      if (onHistoryUpdate) onHistoryUpdate(value.buffer);
    });
    
    return true;
  } catch (e) {
    console.error(e);
    return false;
  }
}

// Prisilno gasi aktivnu BLE konekciju i resetuje deviceId.
// Poziva se iz watchdoga u main.js kada live paketi prestanu da stižu
// a deviceId je i dalje setovan (tihi pad BLE steka).
export async function forceDisconnect() {
  if (!deviceId) return;
  const id = deviceId;
  deviceId = null; // resetuj odmah da auto-reconnect može da krene
  try {
    await BleClient.disconnect(id);
  } catch (e) {
    // Ignorisati — konekcija je verovatno već mrtva
    console.warn('forceDisconnect: BleClient.disconnect bacio grešku (očekivano ako je veza već pala):', e);
  }
}

export async function sendCommand(char) {
  if (!deviceId) return;
  await BleClient.write(deviceId, SERVICE_UUID, LIVE_UUID, numbersToDataView([char.charCodeAt(0)]));
}

// Manual WiFi radio switch on the device; firmware never toggles this automatically.
export async function setWifiEnabled(enabled) {
  if (!deviceId) return;
  await BleClient.write(deviceId, SERVICE_UUID, LIVE_UUID, numbersToDataView(['W'.charCodeAt(0), enabled ? 1 : 0]));
}

// Sends the current phone accurate local epoch to initialize the ESP32 RTC system immediately.
export async function sendTime(epoch) {
  if (!deviceId) return;
  const val = epoch >>> 0;
  const bytes = [
    'T'.charCodeAt(0),
    val & 0xff,
    (val >>> 8) & 0xff,
    (val >>> 16) & 0xff,
    (val >>> 24) & 0xff,
  ];
  await BleClient.write(deviceId, SERVICE_UUID, LIVE_UUID, numbersToDataView(bytes));
}

// Firmware listens for 'G' on the history characteristic (not the live one), so history requests must target it.
// sinceEpoch (optional): only points newer than this unix timestamp are streamed back; 0 requests full history.
export async function requestHistory(sinceEpoch = 0) {
  if (!deviceId) return;
  const since = sinceEpoch >>> 0;
  const bytes = [
    'G'.charCodeAt(0),
    since & 0xff,
    (since >>> 8) & 0xff,
    (since >>> 16) & 0xff,
    (since >>> 24) & 0xff,
  ];
  await BleClient.write(deviceId, SERVICE_UUID, HISTORY_UUID, numbersToDataView(bytes));
}

// Asks the station to rewrite its stored range with a linear ramp, so the fix is shared with every client.
export async function interpolateHistory(startEpoch, endEpoch) {
  if (!deviceId) return;
  const a = startEpoch >>> 0, b = endEpoch >>> 0;
  const bytes = [
    'I'.charCodeAt(0),
    a & 0xff, (a >>> 8) & 0xff, (a >>> 16) & 0xff, (a >>> 24) & 0xff,
    b & 0xff, (b >>> 8) & 0xff, (b >>> 16) & 0xff, (b >>> 24) & 0xff,
  ];
  await BleClient.write(deviceId, SERVICE_UUID, HISTORY_UUID, numbersToDataView(bytes));
}
