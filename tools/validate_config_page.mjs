import fs from "node:fs";

const pagePath = process.argv[2];
if (!pagePath) {
  throw new Error("usage: node validate_config_page.mjs <config_page.html>");
}

const html = fs.readFileSync(pagePath, "utf8");
const scripts = [...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/gi)];
if (scripts.length !== 1) {
  throw new Error(`expected one inline script, found ${scripts.length}`);
}
if (/<script[^>]+src\s*=|<link[^>]+href\s*=/i.test(html)) {
  throw new Error("configuration page must not depend on external assets");
}

// Parsing is enough here; DOM/network behavior is exercised on the ESP32 page.
new Function(scripts[0][1]);
console.log(`OK: inline JavaScript parsed; ${Buffer.byteLength(html)} HTML bytes; no external assets`);
