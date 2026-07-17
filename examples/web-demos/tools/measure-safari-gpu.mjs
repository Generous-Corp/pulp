#!/usr/bin/env node
// measure-safari-gpu.mjs — drive REAL Safari (safaridriver / WebDriver) to verify a
// Pulp WebGPU-audio demo on the actual engine, on the real GPU.
//
// WHY THIS EXISTS (do not re-learn it the hard way):
//   • Playwright's bundled "WebKit" is a STRIPPED build with NO WebGPU
//     (`navigator.gpu` is undefined), so it can never verify the GPU audio path.
//     safaridriver drives the REAL Safari.app, which HAS WebGPU.
//   • Repeated HEADLESS-Chrome (Dawn) WebGPU runs wedge that context's GPU for the
//     session (headless produces 0 blocks and stays that way). Real Safari runs in
//     its own process on the system GPU and is NOT wedged by that — so when headless
//     Chrome goes dark mid-session, THIS still works.
//   • This Mac may be too FAST to reproduce a user's miss rate (0% miss / low
//     round-trip here vs 37% on a slower device). Use this to verify CORRECTNESS
//     (does the GPU lane produce? does a change keep audio correct? does the stat
//     appear?), and inject misses deterministically in the native stub-GPU harness
//     (test/test_super_convolver_web_gpu.cpp) to verify miss-rate LOGIC.
//
// PREREQS (one-time, macOS): `sudo safaridriver --enable`, and Safari →
//   Develop → "Allow Remote Automation". `npm i selenium-webdriver` in this dir tree.
//
// AUDIO ETIQUETTE: this opens a real Safari window and starts the demo, so the synth
//   loop plays out the speakers. Announce before running, keep it capped, and it
//   quits Safari (closing the window) on exit. See CLAUDE.md local-dev audio etiquette.
//
// Usage: node measure-safari-gpu.mjs --url <demo-url> [--secs 20]
import { Builder, By, until, Key } from "selenium-webdriver";

const arg = (k, d) => { const i = process.argv.indexOf(k); return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : d; };
const url = arg("--url", null);
if (!url) { console.error("measure-safari-gpu: --url is required"); process.exit(2); }
const secs = parseInt(arg("--secs", "20"), 10);

const driver = await new Builder().forBrowser("safari").build();
try {
  await driver.get(url);
  // START: a WebDriver .click() on #ov-start (or #overlay, where start() is bound) does
  // NOT fire start() in safaridriver — the overlay never hides. A trusted KEYPRESS does:
  // focus the play button and send Enter, which its keydown handler routes to start()
  // (and a WebDriver keypress is a user gesture, so AudioContext.resume() is allowed).
  const ov = await driver.wait(until.elementLocated(By.id("ov-start")), 25000);
  await ov.click().catch(() => {});
  await ov.sendKeys(Key.RETURN).catch(async () => { await ov.sendKeys(" ").catch(() => {}); });
  // Cold mount: DSP + WCLAP + WebGPU bring-up + a multi-MB UI wasm. Allow generous time.
  await driver.wait(until.elementLocated(By.id("pulp-ui-canvas")), 45000);
  await driver.sleep(5000);
  // ENGAGE THE GPU ENGINE by setting the param directly — NOT by clicking the canvas
  // chip. The editor uses a design-viewport transform (pinned/letterboxed), so canvas
  // CSS coords do not map to the chip's rect and the click misses. window.__player.setParam
  // wraps the adapter's setParameterValue; the Engine param id comes from window.__demo.params.
  const set = await driver.executeScript(() => {
    const ps = (window.__demo && window.__demo.params) || [];
    const eng = ps.find((p) => /engine/i.test(p.name || p.label || ""));
    if (!eng || !window.__player) return { ok: false };
    window.__player.setParam(eng.id, eng.max != null ? eng.max : 1);
    return { ok: true, id: eng.id };
  });
  if (!set.ok) console.log(`${url}\n  could not set Engine=GPU (no param / no __player)`);
  await driver.sleep(4000);
  const s0 = await driver.executeScript(() => window.__gpuStats || null);
  await driver.sleep(secs * 1000);
  const s1 = await driver.executeScript(() => window.__gpuStats || null);
  if (!s0 || !s1) { console.log(`${url}\n  window.__gpuStats null — GPU lane not producing (engine not set? blocked?)`); }
  else {
    const dP = (s1.produced || 0) - (s0.produced || 0), dM = (s1.miss || 0) - (s0.miss || 0), t = dP + dM;
    // appliedLatency / recommendedDepth exist on the adaptive-depth build; harmless when absent.
    const adaptive = s1.appliedLatency != null ? `  appliedDepth=${s1.appliedLatency}  recommended=${s1.recommendedDepth}` : "";
    console.log(`${url}`);
    console.log(`  produced=${dP}  missed=${dM}  missRate=${t ? (dM / t * 100).toFixed(1) : "0.0"}%  roundTrip=${Math.round(s1.avgBlockUs || 0)}µs  gpuNsLast=${s1.gpuNsLast || 0}${adaptive}`);
  }
} finally {
  await driver.quit();
}
