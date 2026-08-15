# AHT BLE WebOTA

Arduino sketch for an ESP32-based temperature and humidity sensor node with BLE telemetry, serial output, persistent snapshots, and WebOTA updates.

## Hardware

- AHT sensor on I2C: SDA `22`, SCL `19`
- BQ25895 support is temporarily disabled with `ENABLE_BQ 0`

## Measurements and snapshots

- Serial output runs at `115200` baud and prints temperature and relative humidity every three seconds.
- Connect to the `VoiceToysWS-OTA` Wi-Fi network and open `http://192.168.4.1:8081/`.
- The page shows live temperature and humidity and can save up to 24 snapshots in ESP32 non-volatile storage.
- Each snapshot stores browser time and an editable comment.
- Browser location is stored when the browser exposes geolocation and the user grants permission; otherwise the snapshot is saved without it.

## BLE

- Device name: `VoiceToysWS`
- Service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- Characteristic UUID: `beb5483e-36e1-4688-b7f5-ea07361b26a8`

The characteristic:

- sends notifications with ASCII payload like `T:24.10,H:51.20,V:0.00V` while BQ support is disabled
- accepts BLE writes
- starts OTA mode when it receives ASCII character `U`

## OTA update flow

1. Connect to the device over BLE.
2. Write ASCII character `U` to the characteristic.
3. The ESP32 starts a Wi-Fi access point named `VoiceToysWS-OTA`.
4. Connect to that Wi-Fi network.
5. Open `http://192.168.4.1:8080/webota` in a browser.
6. Upload the compiled firmware `.bin` file.

## Files

- `HTU21D.ino` - main sketch

## Notes

- Wi-Fi stays off until OTA is triggered over BLE.
- USB Serial debug output includes a millisecond timestamp.
- Build artifacts are ignored by Git.