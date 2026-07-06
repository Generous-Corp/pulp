// Headless validation of the WAMv2 browser fixture.
//
// Serves the assembled fixture and drives index.html in headless Chrome/Chromium
// via playwright-core (pointing at a system/CI browser — no browser download).
// The page loads the WAM through the full AudioWorklet path, renders into an
// OfflineAudioContext, builds its generated GUI, and records the outcome in
// `window.__result` ({ steps:[{name,pass,detail}], pass }). This asserts every
// step passed, so the browser proof runs in CI instead of only by hand.
//
// Prerequisites (the CI lane / README does these): build PulpGainWorklet-wam and
// copy the served files (wam-dsp.js, wam-processor.js, wam-runtime.mjs,
// wam-plugin.js) next to index.html.
//
// Usage: node validate.mjs [--browser <path>] [--screenshot <png>] [--headed]
// Exit 0 = PASS.
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { existsSync } from "node:fs";
import { chromium } from "playwright-core";

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const screenshot = arg("--screenshot", null);
const headed = process.argv.includes("--headed");
const PORT = 8731;
const PAGE = `http://localhost:${PORT}/`;

const CANDIDATES = [
  arg("--browser", null),
  process.env.PLAYWRIGHT_CHROMIUM_PATH,
  process.env.CHROME_PATH,
  "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium-browser",
  "/usr/bin/chromium",
].filter(Boolean);

const fail = (m) => { console.error("FAIL: " + m); process.exitCode = 1; };

const here = new URL(".", import.meta.url).pathname;
if (!existsSync(here + "wam-dsp.js") || !existsSync(here + "wam-plugin.js")) {
  fail("fixture not assembled — build PulpGainWorklet-wam and copy wam-dsp.js / wam-*.js next to index.html (see README).");
  process.exit(1);
}

const server = spawn(process.execPath, [here + "serve.mjs"], { stdio: ["ignore", "pipe", "inherit"] });
await sleep(400);

let browser;
try {
  const exe = CANDIDATES.find((p) => existsSync(p));
  if (!exe) { fail("no Chrome/Chromium binary found (set CHROME_PATH or --browser)"); }
  else {
    browser = await chromium.launch({ executablePath: exe, headless: !headed });
    const page = await browser.newPage();
    page.on("console", (m) => console.log("  [page]", m.text()));
    page.on("pageerror", (e) => console.log("  [pageerror]", e.message));
    await page.goto(PAGE, { waitUntil: "load" });

    await page.waitForFunction(() => window.__result && (window.__result.pass || window.__result.error || window.__result.steps.length >= 4),
      null, { timeout: 30000 });
    const result = await page.evaluate(() => window.__result);
    for (const s of result.steps) console.log(`  ${s.pass ? "ok  " : "FAIL"} ${s.name}${s.detail ? " — " + s.detail : ""}`);
    if (result.error) console.log("  error:", result.error);
    if (screenshot) { await page.screenshot({ path: screenshot }); console.log("screenshot:", screenshot); }

    if (!result.pass) throw new Error("one or more browser checks failed");
    console.log("PASS: WAMv2 plugin loaded and rendered in the browser (all checks passed)");
  }
} catch (e) {
  fail(String(e && e.message ? e.message : e));
} finally {
  if (browser) await browser.close();
  server.kill("SIGTERM");
}
