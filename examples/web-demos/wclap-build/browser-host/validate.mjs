// Headless validation of the WebCLAP browser host.
//
// Starts the COOP/COEP dev server, drives the page in headless Chrome/Canary via
// playwright-core (pointing at a system browser — no browser download), and
// asserts the WebCLAP plugin is hosted in the browser: it activates, renders
// audio at unity (passthrough), and responds to a parameter change driven
// through a generated UI control (raising Input Gain +6 dB lifts the output
// ~+6 dB). Writes a screenshot of the UI.
//
// Usage:
//   node validate.mjs [--browser <path>] [--screenshot <png>] [--headed]
// Exit 0 = PASS. Requires PulpGain.wasm built (examples/web-demos/wclap-build).
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { chromium } from "playwright-core";

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const CANDIDATES = [
  arg("--browser", null),
  process.env.PLAYWRIGHT_CHROMIUM_PATH,
  process.env.CHROME_PATH,
  "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium-browser",
  "/usr/bin/chromium",
].filter(Boolean);
const screenshot = arg("--screenshot", null);
const headed = process.argv.includes("--headed");
const PORT = 8788;
const PAGE = `http://localhost:${PORT}/examples/web-demos/wclap-build/browser-host/`;

const fail = (m) => { console.error("FAIL: " + m); process.exitCode = 1; };

const server = spawn(process.execPath,
  [new URL("./serve.mjs", import.meta.url).pathname, String(PORT)],
  { stdio: ["ignore", "pipe", "inherit"] });
await sleep(400);

let browser;
try {
  const fs = await import("node:fs");
  const exe = CANDIDATES.find((p) => fs.existsSync(p));
  if (!exe) { fail("no Chrome/Canary/Chromium binary found"); }
  else {
    browser = await chromium.launch({ executablePath: exe, headless: !headed });
    const page = await browser.newPage();
    page.on("console", (m) => console.log("  [page]", m.text()));
    page.on("pageerror", (e) => console.log("  [pageerror]", e.message));
    await page.goto(PAGE, { waitUntil: "load" });

    // Wait for the host to finish booting (ready flag or error).
    await page.waitForFunction(() => window.__wclapReady || window.__wclapError, null, { timeout: 15000 });
    const err = await page.evaluate(() => window.__wclapError);
    if (err) throw new Error("page reported: " + err);

    const dflt = await page.evaluate(() => window.__wclapLast);
    console.log(`default render: in=${dflt.inRms.toFixed(3)} out=${dflt.outRms.toFixed(3)} Δ=${dflt.deltaDb.toFixed(2)}dB`);
    console.log("params:", dflt.params.map((p) => `${p.name}=${p.value}`).join(", "));
    if (!(Math.abs(dflt.deltaDb) < 0.5)) throw new Error(`default render is not unity passthrough (Δ=${dflt.deltaDb.toFixed(2)}dB)`);

    // Drive the "Input Gain" generated control to +6 dB and re-render.
    await page.evaluate(() => {
      const rows = [...document.querySelectorAll(".param")];
      const row = rows.find((r) => /input gain/i.test(r.querySelector("label").textContent));
      const range = row.querySelector("input[type=range]");
      range.value = "6";
      range.dispatchEvent(new Event("input", { bubbles: true }));
    });
    await page.waitForFunction(() => {
      const g = window.__wclapLast?.params?.find((p) => /input gain/i.test(p.name));
      return g && Math.abs(g.value - 6) < 0.01;
    }, null, { timeout: 5000 });
    const gained = await page.evaluate(() => window.__wclapLast);
    const rise = gained.deltaDb - dflt.deltaDb;
    console.log(`after Input Gain=+6: out=${gained.outRms.toFixed(3)} Δ=${gained.deltaDb.toFixed(2)}dB (rise ${rise.toFixed(2)}dB)`);

    if (screenshot) { await page.screenshot({ path: screenshot }); console.log("screenshot:", screenshot); }

    if (!(rise > 4.5 && rise < 7.5)) throw new Error(`parameter change did not raise output ~6 dB (rise ${rise.toFixed(2)}dB)`);
    console.log("PASS: WebCLAP plugin hosted in the browser — activates, renders audio, responds to a parameter control");
  }
} catch (e) {
  fail(String(e && e.message ? e.message : e));
} finally {
  if (browser) await browser.close();
  server.kill("SIGTERM");
}
