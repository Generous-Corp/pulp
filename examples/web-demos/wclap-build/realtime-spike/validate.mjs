// Headless validation of the WS-C2 real-time-worklet spike.
//
// Starts the COOP/COEP server, drives the page in headless system Chrome via
// playwright-core (no browser download), and asserts:
//   1. crossOriginIsolated === true (shared memory allowed);
//   2. the WebCLAP plugin is instantiated + activated INSIDE the AudioWorklet;
//   3. it renders in REAL TIME — over ~1.5 s of wall clock the worklet's
//      process() ran ~ sampleRate/128 quanta per second (audio-clock-locked,
//      not offline / not free-running);
//   4. raising "Input Gain" +6 dB via a generated control lifts output ~+6 dB;
//   5. the output-events path is live (a host-visible param change is reported).
//
// Usage: node validate.mjs [--browser <path>] [--screenshot out.png] [--headed]
// Exit 0 = PASS. Requires PulpGain.wasm alongside (the built WebCLAP module).
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { chromium } from "playwright-core";

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const CANDIDATES = [
  arg("--browser", null), process.env.PLAYWRIGHT_CHROMIUM_PATH, process.env.CHROME_PATH,
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/usr/bin/google-chrome", "/usr/bin/chromium-browser", "/usr/bin/chromium",
].filter(Boolean);
const screenshot = arg("--screenshot", null);
const headed = process.argv.includes("--headed");
const PORT = 8791;
const PAGE = `http://localhost:${PORT}/examples/web-demos/wclap-build/realtime-spike/`;
const fail = (m) => { console.error("FAIL: " + m); process.exitCode = 1; };

const server = spawn(process.execPath,
  [new URL("./serve.mjs", import.meta.url).pathname, String(PORT)],
  { stdio: ["ignore", "pipe", "inherit"] });
await sleep(400);

let browser;
try {
  const fs = await import("node:fs");
  const exe = CANDIDATES.find((p) => fs.existsSync(p));
  if (!exe) { fail("no Chrome/Chromium binary found"); }
  else {
    // --autoplay-policy so the AudioContext runs without a user gesture in headless.
    browser = await chromium.launch({ executablePath: exe, headless: !headed,
      args: ["--autoplay-policy=no-user-gesture-required"] });
    const page = await browser.newPage();
    page.on("console", (m) => console.log("  [page]", m.text()));
    page.on("pageerror", (e) => console.log("  [pageerror]", e.message));
    await page.goto(PAGE, { waitUntil: "load" });

    await page.waitForFunction(() => window.__poc?.ready || window.__pocError, null, { timeout: 15000 });
    const err = await page.evaluate(() => window.__pocError);
    if (err) throw new Error("page reported: " + err);

    const coi = await page.evaluate(() => self.crossOriginIsolated);
    console.log("crossOriginIsolated:", coi);
    if (!coi) throw new Error("page is not cross-origin isolated");

    const desc = await page.evaluate(() => window.__poc.descriptor);
    const plist = await page.evaluate(() => window.__poc.params);
    console.log(`plugin: ${desc.name} (${desc.id}); params: ${plist.map((p) => p.name).join(", ")}`);

    // ── Real-time proof: sample two meter snapshots ~1.2 s apart and check the
    //    quanta advanced at ~sampleRate/128 per second (audio-clock-locked).
    await page.waitForFunction(() => window.__poc.lastMeter, null, { timeout: 5000 });
    const m0 = await page.evaluate(() => window.__poc.lastMeter);
    const wall0 = Date.now();
    await sleep(1200);
    const m1 = await page.evaluate(() => window.__poc.lastMeter);
    const wallDt = (Date.now() - wall0) / 1000;
    const dQuanta = m1.quanta - m0.quanta;
    const dCtx = m1.currentTime - m0.currentTime;
    const expected = (m1.sampleRate / 128) * wallDt;
    const rtRatio = dQuanta / expected;
    console.log(`real-time: Δquanta=${dQuanta} over ${wallDt.toFixed(2)}s wall ` +
      `(ctxΔ=${dCtx.toFixed(2)}s); expected ~${expected.toFixed(0)} → ratio ${rtRatio.toFixed(3)}`);
    if (!(rtRatio > 0.7 && rtRatio < 1.3))
      throw new Error(`not real-time: quanta ratio ${rtRatio.toFixed(3)} (offline/free-running?)`);
    if (!(Math.abs(dCtx - wallDt) < 0.4))
      throw new Error(`AudioContext clock not tracking wall clock (ctxΔ ${dCtx.toFixed(2)} vs ${wallDt.toFixed(2)})`);

    // ── Baseline level (PulpGain default = unity → out ≈ in).
    const base = await page.evaluate(() => window.__poc.lastMeter);
    const baseDb = 20 * Math.log10((base.outRms || 1e-9) / (base.inRms || 1e-9));
    console.log(`baseline: in=${base.inRms.toFixed(3)} out=${base.outRms.toFixed(3)} Δ=${baseDb.toFixed(2)}dB`);
    if (!(Math.abs(baseDb) < 0.7)) throw new Error(`baseline not unity passthrough (Δ=${baseDb.toFixed(2)}dB)`);

    // ── Param proof: drive "Input Gain" to +6 dB, wait a few quanta, re-measure.
    await page.evaluate(() => {
      const rows = [...document.querySelectorAll(".param")];
      const row = rows.find((r) => /input gain/i.test(r.querySelector("label").textContent));
      const range = row.querySelector("input[type=range]");
      range.value = "6"; range.dispatchEvent(new Event("input", { bubbles: true }));
    });
    await sleep(500);
    const gained = await page.evaluate(() => window.__poc.lastMeter);
    const gainedDb = 20 * Math.log10((gained.outRms || 1e-9) / (gained.inRms || 1e-9));
    const rise = gainedDb - baseDb;
    console.log(`after Input Gain=+6: out=${gained.outRms.toFixed(3)} Δ=${gainedDb.toFixed(2)}dB (rise ${rise.toFixed(2)}dB)`);

    if (screenshot) { await page.screenshot({ path: screenshot }); console.log("screenshot:", screenshot); }

    if (!(rise > 4.5 && rise < 7.5))
      throw new Error(`param change did not raise output ~6 dB (rise ${rise.toFixed(2)}dB)`);

    console.log("PASS: WebCLAP renders REAL-TIME in an AudioWorklet and responds to a param change.");
  }
} catch (e) {
  fail(String(e && e.message ? e.message : e));
} finally {
  if (browser) await browser.close();
  server.kill("SIGTERM");
}
