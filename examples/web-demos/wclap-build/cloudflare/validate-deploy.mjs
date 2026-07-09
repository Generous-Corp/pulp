// LOCAL PROOF that the assembled Cloudflare deploy dir works standalone.
//
// Serves ./public through serve-headers.mjs (which applies the real `_headers`
// file), drives it in headless system Chrome via playwright-core, and asserts
// the two acceptance criteria for WS-C1:
//
//   1. window.crossOriginIsolated === true  (COOP/COEP from `_headers` took).
//   2. The WebCLAP PulpGain module loads and renders audio, and a parameter
//      change lifts output ~+6 dB — mirroring browser-host/validate.mjs and the
//      Node wclap_host_runner.mjs reference.
//
// It also verifies EVERY subresource returned HTTP 200 — a COEP: require-corp
// block (missing CORP) shows up as a failed/blocked request, so a green run
// proves no subresource was rejected under the isolation headers.
//
// Usage:
//   node validate-deploy.mjs [--browser <path>] [--screenshot <png>] [--headed]
// Exit 0 = PASS. Requires ./public assembled (run assemble.mjs first) and
// playwright-core installed (npm install --no-save playwright-core).
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { existsSync } from "node:fs";
import { chromium } from "playwright-core";

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const CANDIDATES = [
  arg("--browser", null),
  process.env.PLAYWRIGHT_CHROMIUM_PATH,
  process.env.CHROME_PATH,
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium-browser",
  "/usr/bin/chromium",
].filter(Boolean);
const screenshot = arg("--screenshot", null);
const headed = process.argv.includes("--headed");
const PORT = Number(arg("--port", 8791));
const PAGE = `http://localhost:${PORT}/`;

const fail = (m) => { console.error("FAIL: " + m); process.exitCode = 1; };

const server = spawn(process.execPath,
  [new URL("./serve-headers.mjs", import.meta.url).pathname, "--port", String(PORT)],
  { stdio: ["ignore", "pipe", "inherit"] });
await sleep(400);

let browser;
try {
  const exe = CANDIDATES.find((p) => p && existsSync(p));
  if (!exe) { fail("no Chrome/Chromium binary found"); }
  else {
    browser = await chromium.launch({ executablePath: exe, headless: !headed });
    const page = await browser.newPage();
    const responses = []; // every subresource response status
    page.on("response", (r) => responses.push({ url: r.url(), status: r.status() }));
    page.on("requestfailed", (r) =>
      responses.push({ url: r.url(), status: 0, error: r.failure()?.errorText }));
    page.on("console", (m) => console.log("  [page]", m.text()));
    page.on("pageerror", (e) => console.log("  [pageerror]", e.message));
    await page.goto(PAGE, { waitUntil: "load" });

    // (1) cross-origin isolation.
    const coi = await page.evaluate(() => self.crossOriginIsolated === true);
    console.log(`crossOriginIsolated = ${coi}`);
    if (!coi) throw new Error("page is NOT crossOriginIsolated — COOP/COEP headers did not apply");

    // (2) WebCLAP module hosted + audible.
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

    // (3) every subresource loaded (no COEP block).
    const bad = responses.filter((r) => r.status !== 200 && !r.url.startsWith("data:"));
    console.log("subresources:");
    for (const r of responses.filter((r) => !r.url.startsWith("data:"))) {
      console.log(`  ${r.status || "ERR"}  ${r.url.replace(PAGE, "/")}${r.error ? "  (" + r.error + ")" : ""}`);
    }
    if (bad.length) throw new Error(`${bad.length} subresource(s) did not return 200 (COEP block / 404): ` +
      bad.map((r) => `${r.url} → ${r.status}${r.error ? " " + r.error : ""}`).join("; "));

    if (!(rise > 4.5 && rise < 7.5)) throw new Error(`parameter change did not raise output ~6 dB (rise ${rise.toFixed(2)}dB)`);
    console.log("PASS: crossOriginIsolated + WebCLAP audio + all subresources 200 under _headers");
  }
} catch (e) {
  fail(String(e && e.message ? e.message : e));
} finally {
  if (browser) await browser.close();
  server.kill("SIGTERM");
}
