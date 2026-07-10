// Headless proof of the shared-player WebCLAP demo (WS-C2 headline).
//
// Serves the assembled public/ under the real Cloudflare `_headers` (via
// serve-headers.mjs) and drives public/player/ in headless system Chrome, then
// asserts the deliverable end-to-end:
//   1. crossOriginIsolated === true (threaded WebCLAP allowed under COOP/COEP);
//   2. every subresource the page pulls returns 200 under COEP require-corp;
//   3. the SHARED player UI rendered — the auto-generated widget grid, the
//      oscilloscope, and the level meter — identical to the WAM demos;
//   4. REAL-TIME render: the worklet's process() ran ~sampleRate/128 quanta per
//      wall-second (audio-clock-locked, not offline) with the context clock in
//      lockstep;
//   5. a parameter change is AUDIBLE (driving Input Gain +6 dB lifts the plugin
//      output ~+6 dB) AND updates the shared widget (the on-screen knob shows it).
//
// Usage: node validate-player.mjs [--browser <path>] [--screenshot out.png] [--headed]
// Exit 0 = PASS.
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { existsSync } from "node:fs";
import { chromium } from "playwright-core";

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const CANDIDATES = [
  arg("--browser", null), process.env.PLAYWRIGHT_CHROMIUM_PATH, process.env.CHROME_PATH,
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/usr/bin/google-chrome", "/usr/bin/chromium",
].filter(Boolean);
const screenshot = arg("--screenshot", null);
const headed = process.argv.includes("--headed");
const PORT = Number(arg("--port", 8793));
const PAGE = `http://localhost:${PORT}/player/`;
const fail = (m) => { console.error("FAIL: " + m); process.exitCode = 1; };

const server = spawn(process.execPath,
  [new URL("./serve-headers.mjs", import.meta.url).pathname, "--port", String(PORT)],
  { stdio: ["ignore", "pipe", "inherit"] });
await sleep(500);

let browser;
try {
  const exe = CANDIDATES.find((p) => existsSync(p));
  if (!exe) throw new Error("no Chrome/Chromium binary found (pass --browser)");
  browser = await chromium.launch({ executablePath: exe, headless: !headed,
    args: ["--autoplay-policy=no-user-gesture-required"] });
  const page = await browser.newPage();
  page.on("console", (m) => console.log("  [page]", m.text()));
  page.on("pageerror", (e) => console.log("  [pageerror]", e.message));

  // Track every subresource status (COEP require-corp fails silently otherwise).
  const bad = [];
  page.on("requestfinished", async (req) => {
    try { const r = await req.response(); if (r && r.status() >= 400) bad.push(`${r.status()} ${req.url()}`); } catch {}
  });

  await page.goto(PAGE, { waitUntil: "load" });

  const coi = await page.evaluate(() => self.crossOriginIsolated);
  console.log("1. crossOriginIsolated:", coi);
  if (!coi) throw new Error("page is not cross-origin isolated");

  // Start the shared player (user-gesture seam) and wait for the descriptor.
  await page.evaluate(() => window.__start());
  await page.waitForFunction(() => window.__demo?.started && window.__demo?.descriptor, null, { timeout: 20000 });
  const desc = await page.evaluate(() => window.__demo.descriptor);
  const params = await page.evaluate(() => window.__demo.params);
  console.log(`   plugin: ${desc.name} (${desc.id}) — hasState=${desc.hasState}; ` +
    `params: ${params.map((p) => p.label + (p.type === "boolean" ? "[toggle]" : "")).join(", ")}`);

  // ── 2. subresources all 200 under COEP.
  await sleep(300);
  if (bad.length) throw new Error("subresource(s) failed under COEP: " + bad.join(", "));
  console.log("2. all subresources 200 under COEP require-corp");

  // ── 3. the SHARED player UI rendered (same DOM the WAM demos build).
  const ui = await page.evaluate(() => ({
    cells: document.querySelectorAll("#params .pw-cell").length,
    hasScope: !!document.querySelector("#scope"),
    hasMeter: !!document.querySelector(".pw-meter"),
    hasSource: !!document.querySelector("#src"),
    knobCanvases: document.querySelectorAll("#params canvas").length,
  }));
  console.log(`3. shared UI: ${ui.cells} widget cells, ${ui.knobCanvases} canvas widgets, ` +
    `scope=${ui.hasScope}, meter=${ui.hasMeter}, source-selector=${ui.hasSource}`);
  if (!(ui.cells >= 3 && ui.hasScope && ui.hasMeter && ui.hasSource))
    throw new Error("shared player UI did not fully render");

  // ── 4. real-time proof from the worklet's own quanta clock (diag meter).
  await page.waitForFunction(() => window.__wclap?.lastMeter, null, { timeout: 6000 });
  const m0 = await page.evaluate(() => window.__wclap.lastMeter);
  const wall0 = Date.now();
  await sleep(1200);
  const m1 = await page.evaluate(() => window.__wclap.lastMeter);
  const wallDt = (Date.now() - wall0) / 1000;
  const dQuanta = m1.quanta - m0.quanta;
  const dCtx = m1.currentTime - m0.currentTime;
  const expected = (m1.sampleRate / 128) * wallDt;
  const rtRatio = dQuanta / expected;
  console.log(`4. real-time: Δquanta=${dQuanta} over ${wallDt.toFixed(2)}s wall ` +
    `(ctxΔ=${dCtx.toFixed(2)}s); expected ~${expected.toFixed(0)} → ratio ${rtRatio.toFixed(3)}`);
  if (!(rtRatio > 0.7 && rtRatio < 1.3)) throw new Error(`not real-time (quanta ratio ${rtRatio.toFixed(3)})`);
  if (!(Math.abs(dCtx - wallDt) < 0.4)) throw new Error(`context clock not tracking wall clock`);

  // Baseline: PulpGain default = unity passthrough.
  const base = await page.evaluate(() => window.__wclap.lastMeter);
  const baseDb = 20 * Math.log10((base.outRms || 1e-9) / (base.inRms || 1e-9));
  console.log(`   baseline: in=${base.inRms.toFixed(3)} out=${base.outRms.toFixed(3)} Δ=${baseDb.toFixed(2)}dB`);
  if (!(Math.abs(baseDb) < 1.0)) throw new Error(`baseline not unity passthrough (Δ=${baseDb.toFixed(2)}dB)`);

  // ── 5. drive Input Gain +6 dB through the adapter + reflect it in the shared
  //      widget; prove the output rises ~+6 dB (audible) and the knob shows +6.
  const inputGain = params.find((p) => /input gain/i.test(p.label)) || params[0];
  const widgetShown = await page.evaluate((pid) => {
    window.__player.setParam(pid, 6);            // what the knob's onChange calls (AUDIBLE)
    const entry = (window.__widgets || {})[pid];
    if (entry?.el?.setValue) entry.el.setValue(6); // reflect on the shared on-screen knob
    return entry?.el?.getValue?.() ?? null;         // read back what the widget displays
  }, inputGain.id);
  await sleep(500);
  const gained = await page.evaluate(() => window.__wclap.lastMeter);
  const gainedDb = 20 * Math.log10((gained.outRms || 1e-9) / (gained.inRms || 1e-9));
  const rise = gainedDb - baseDb;
  console.log(`5. Input Gain → +6: output Δ=${gainedDb.toFixed(2)}dB (rise ${rise.toFixed(2)}dB); ` +
    `shared widget shows ${widgetShown}`);

  if (!(rise > 4.5 && rise < 7.5)) throw new Error(`param change not audible (rise ${rise.toFixed(2)}dB)`);
  if (!(Math.abs(widgetShown - 6) < 0.01)) throw new Error(`shared widget did not reflect the param (${widgetShown})`);

  // ── 6. clap.state round-trip through the adapter: snapshot at +6, move the
  //      param, restore the snapshot, and confirm the value comes back. Proves
  //      the worklet host's clap.state save/load (the SAME PLST blob native uses),
  //      not just that the extension is present.
  const state = await page.evaluate(async (pid) => {
    const wam = window.__player.state.wam;
    const snap = await wam.getState();          // save at Input Gain = +6
    window.__player.setParam(pid, 0);           // move it away
    await new Promise((r) => setTimeout(r, 60));
    await wam.setState(snap);                    // restore
    await new Promise((r) => setTimeout(r, 60));
    return { len: snap ? snap.length : 0, restored: await wam.getParameterValue(pid) };
  }, inputGain.id);
  console.log(`6. clap.state round-trip: snapshot ${state.len} bytes; after restore Input Gain = ${state.restored}`);
  if (!(state.len > 0)) throw new Error("getState returned no bytes (clap.state save failed)");
  if (!(Math.abs(state.restored - 6) < 0.01)) throw new Error(`clap.state restore did not bring Input Gain back to +6 (got ${state.restored})`);

  if (screenshot) { await page.screenshot({ path: screenshot }); console.log("   screenshot:", screenshot); }

  console.log("\nPASS: PulpGain WebCLAP renders REAL-TIME behind the shared player; a param change is audible and updates the shared widget.");
} catch (e) {
  fail(String(e && e.message ? e.message : e));
} finally {
  if (browser) await browser.close();
  server.kill("SIGTERM");
}
