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
import { Builder, By, until, Origin } from "selenium-webdriver";

const arg = (k, d) => { const i = process.argv.indexOf(k); return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : d; };
const url = arg("--url", null);
if (!url) { console.error("measure-safari-gpu: --url is required"); process.exit(2); }
const secs = parseInt(arg("--secs", "20"), 10);

const driver = await new Builder().forBrowser("safari").build();
try {
  await driver.get(url);
  await driver.wait(until.elementLocated(By.id("ov-start")), 25000);
  await driver.findElement(By.id("ov-start")).click();   // real gesture → AudioContext resumes
  await driver.wait(until.elementLocated(By.id("pulp-ui-canvas")), 25000);
  await driver.sleep(6000);
  const rect = await driver.executeScript(() => {
    const c = document.getElementById("pulp-ui-canvas"); const r = c.getBoundingClientRect();
    return { x: r.left, y: r.top, w: r.width, h: r.height };
  });
  // Engage the GPU engine: real click on the top-right engine chip.
  const chipX = Math.round(rect.x + rect.w - 100), chipY = Math.round(rect.y + 30);
  await driver.actions({ async: true }).move({ x: chipX, y: chipY, origin: Origin.VIEWPORT }).click().perform();
  await driver.sleep(4000);
  const s0 = await driver.executeScript(() => window.__gpuStats || null);
  await driver.sleep(secs * 1000);
  const s1 = await driver.executeScript(() => window.__gpuStats || null);
  if (!s0 || !s1) { console.log(`${url}\n  window.__gpuStats null — GPU lane not producing (chip miss? blocked?)`); }
  else {
    const dP = (s1.produced || 0) - (s0.produced || 0), dM = (s1.miss || 0) - (s0.miss || 0), t = dP + dM;
    console.log(`${url}`);
    console.log(`  produced=${dP}  missed=${dM}  missRate=${t ? (dM / t * 100).toFixed(1) : "0.0"}%  roundTrip=${Math.round(s1.avgBlockUs || 0)}µs  gpuNsLast=${s1.gpuNsLast || 0}`);
  }
} finally {
  await driver.quit();
}
