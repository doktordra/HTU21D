# VoiceToys Weather Station

Arduino sketch for an ESP32-based temperature and humidity sensor node with BLE telemetry, persistent snapshots, web diagnostics, and WebOTA updates.

## Hardware

- AHT sensor on I2C: SDA `1`, SCL `3`
- BQ25895/BMS on I2C: SDA `33`, SCL `13`
- Two WS2812-compatible smart RGB LEDs on GPIO `14`
- Application UART/Serial is disabled because GPIO `1` and GPIO `3` are used by the AHT bus.
- The ESP32 ROM may still briefly emit its boot message on GPIO `1` before the sketch starts; disabling that requires a separate permanent eFuse configuration and is intentionally not performed by this firmware.

## Dashboard, history, and snapshots

- On first use, connect to the `VoiceToysWS-OTA` Wi-Fi network and open `http://192.168.4.1:8081/`.
- In the **Network** tab, scan for the home Wi-Fi network, enter its password, and save it.
- After the phone reconnects to the same home Wi-Fi, open `http://ws.local:8081/`.
- Up to five networks are stored in ESP32 NVS. At boot, the station selects the strongest available known network.
- If no known network is available, the station opens its `VoiceToysWS-OTA` fallback AP and retries known networks once per minute.
- The page shows temperature, relative humidity, calculated RealFeel, and absolute humidity with smoothed trends and min/max values.
- Graphs share a selectable range from one minute to 24 hours.
- The latest 10 minutes are kept at 500 ms resolution in RAM. One-second samples are retained for 24 hours in a circular SPIFFS file and written in one-minute batches.
- The page can save up to 24 snapshots in ESP32 non-volatile storage.
- Snapshot capture first creates a preview; a separate save action commits the displayed values.
- Each snapshot stores browser time and an editable comment.
- Hold a snapshot row for about one second to delete it.

## Diagnostics

- The **Debug** tab shows AHT presence/read health, BQ25895 communication, battery voltage, BLE client state, WebOTA state, Wi-Fi RSSI, free heap, uptime, and the application core.
- Debug logs remain available through the web interface and the BLE debug characteristic; UART output is not used.
- BQ battery voltage is sampled every 30 seconds to reduce I2C work and preserve WebOTA responsiveness.
- RGB diagnostics use brightness `8/255` and a `120 ms` pulse every three seconds:
	- LED 1: green at or above `3.75 V`, yellow from `3.45 V` to `3.75 V`, red below `3.45 V` or on BQ failure.
	- LED 2: green for a valid AHT reading, red for a missing or invalid sensor reading.
- RGB diagnostics can be disabled persistently from the **Debug** tab.

## BLE

- Device name: `VoiceToysWS`
- Service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- Characteristic UUID: `beb5483e-36e1-4688-b7f5-ea07361b26a8`

The characteristic:

- sends notifications with ASCII payload like `T:24.10,H:51.20,V:3.92V`
- accepts BLE writes
- starts OTA mode when it receives ASCII character `U`

## OTA update flow

1. Open the dashboard through `http://ws.local:8081/` on the home Wi-Fi, or through `http://192.168.4.1:8081/` on the fallback AP.
2. Open the **Network** tab and select **Update firmware**.
3. Upload the compiled firmware `.bin` file.

The BLE `U` command remains available and requests WebOTA startup again.

## Files

- `HTU21D.ino` - main sketch

## Notes

- The local Wi-Fi AP, dashboard, and WebOTA interface start during boot; the BLE `U` command can request WebOTA startup again.
- Wi-Fi/BLE system tasks and the Arduino application already use the ESP32 dual-core runtime; no additional user task is required for the current short I2C/LED operations.
- Build artifacts are ignored by Git.
