const fs = require('fs');
const path = require('path');

// Single source of truth: app/index.html + app/main.js + app/BleController.js,
// built by Vite into one file. Both the firmware dashboard and the Capacitor
// Android app are generated from this same bundle (isNative branches inside
// main.js decide what runs where).
const htmlPath = path.join(__dirname, 'app', 'dist', 'index.html');
const outPath = path.join(__dirname, 'WebInterface.h');
const delimiter = ')=====\"';

if (!fs.existsSync(htmlPath)) {
  console.error("Fajl app/dist/index.html ne postoji. Prvo pokreni 'npm run build' u app/ folderu.");
  process.exit(1);
}

const htmlContent = fs.readFileSync(htmlPath, 'utf8');

if (htmlContent.includes(delimiter)) {
  console.error(`Build sadrzi C++ raw-string delimiter (${delimiter}); promeni delimiter u pack_html.js pre generisanja.`);
  process.exit(1);
}

let headerContent = `// Automatski generisan fajl. Ne menjati rucno!
// Izmeni app/index.html ili app/main.js, pokreni 'npm run build' u app/, pa 'node pack_html.js'

#ifndef WEB_INTERFACE_H
#define WEB_INTERFACE_H

#include <Arduino.h>

const char dashboardHtml[] PROGMEM = R"=====(
${htmlContent}
)=====";

#endif
`;

fs.writeFileSync(outPath, headerContent);
console.log(`Uspesno generisan ${outPath} od ${htmlContent.length} bajtova.`);

