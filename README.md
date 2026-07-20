# HTU21D BLE WebOTA

Arduino sketch for an ESP32-based HTU21D sensor node with BLE telemetry and BLE-triggered WebOTA updates.

## Files

- `HTU21D.ino` - main sketch

## Notes

- OTA server starts only after sending the configured ASCII trigger over BLE.
- Build artifacts are ignored by Git.