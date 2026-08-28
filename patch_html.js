const fs = require('fs');
let html = fs.readFileSync('app/index.html', 'utf8');
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
if (scriptMatch) {
  let scriptContent = scriptMatch[1];
  fs.writeFileSync('app/main.js', scriptContent);
  html = html.replace(/<script>[\s\S]*?<\/script>/, '<script type="module" src="/main.js"></script>');
  fs.writeFileSync('app/index.html', html);
  console.log("Extracted main.js");
} else {
  console.log("No script tag found.");
}
