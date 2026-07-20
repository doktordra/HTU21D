# HTU21D BLE WebOTA

Arduino sketch for an ESP32-based HTU21D sensor node with BLE telemetry, battery voltage reporting, and BLE-triggered WebOTA updates.

## Hardware

- HTU-21D on I2C: SDA `15`, SCL `2`
- BQ25895 on second I2C bus: SDA `33`, SCL `13`

## BLE

- Device name: `VoiceToysWS`
- Service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- Characteristic UUID: `beb5483e-36e1-4688-b7f5-ea07361b26a8`

The characteristic:

- sends notifications with ASCII payload like `T:24.10,H:51.20,V:4.02V`
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
- Build artifacts are ignored by Git.