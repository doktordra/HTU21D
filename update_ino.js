const fs = require('fs');
let code = fs.readFileSync('HTU21D.ino', 'utf8');
code = code.replace('#include <WebServer.h>', '#include <WebServer.h>\n#include "WebInterface.h"');

code = code.replace(/void handleDebugRoot\(\) \{[\s\S]*?debugServer\.sendHeader\("Cache-Control", "no-store"\);/m, 
`void handleDebugRoot() {
  debugServer.sendHeader("Cache-Control", "no-store");`);
  
code = code.replace(/String &html = dashboardHtml;\s*if \(html\.length\(\) == 0\) \{[\s\S]*?\}\s*debugServer\.send/m, 
`debugServer.send`);

code = code.replace('debugServer.send(200, "text/html", html);', 'debugServer.send(200, "text/html", dashboardHtml);');

fs.writeFileSync('HTU21D.ino', code);
