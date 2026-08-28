const fs = require('fs');
let code = fs.readFileSync('HTU21D.ino', 'utf8');

// Remove automatic WiFi off
code = code.replace(/bool charging = false;[\s\S]*?wifiApFallbackActive = false;\n\s*\}/, 'debugLogf("BLE spojen: Ne gasim WiFi automatski, ostavljam kontrolu korisniku.");');

code = code.replace(/if \(WiFi\.getMode\(\) == WIFI_OFF && !otaServerActive\) \{[\s\S]*?\}/, 'debugLogf("BLE otkacen.");');

// Add 'W' command
code = code.replace(/if \(c == 'U'\) \{[\s\S]*?\}\s*\}\s*\}/, 
`if (c == 'U') {
        otaStartRequested = true;
      } else if (c == 'W') {
        if (WiFi.getMode() == WIFI_OFF) {
           debugLogf("Korisnik pali WiFi preko BLE-a.");
           networkReconfigurePending = true;
           networkReconfigureAtMs = millis() + 500;
        } else {
           debugLogf("Korisnik gasi WiFi preko BLE-a.");
           WiFi.mode(WIFI_OFF);
           wifiApFallbackActive = false;
        }
      }
    }
  }`);

fs.writeFileSync('HTU21D.ino', code);
