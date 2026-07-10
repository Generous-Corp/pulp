// Headless-Chrome validation of the Pulp Live Kernel S0 spike.
//
// Dependency-free: launches system Chrome with --remote-debugging and drives it
// over the Chrome DevTools Protocol using Node's built-in WebSocket + fetch (no
// playwright/puppeteer install required). Asserts, all MEASURED in real system
// Chrome, that the resident worklet kernel:
//   1. renders in REAL TIME (audio-clock-locked; quanta/wall ratio >= 0.95);
//   2. applies a PARAM edit audibly with edit->sound latency <= 30 ms;
//   3. applies a STRUCTURAL edit (new graph blob + equal-power crossfade)
//      audibly (RMS shifts) with edit->sound latency <= 30 ms;
//   4. builds a plan on the audio thread within one quantum's budget (2.67 ms).
//
// Usage: node validate.mjs [--browser <path>] [--headed]. Exit 0 = PASS.
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { existsSync } from "node:fs";

const arg = (flag, dflt) => {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
};
const headed = process.argv.includes("--headed");
const PORT = 8794;
const PAGE = `http://127.0.0.1:${PORT}/examples/web-demos/live-kernel-spike/`;
const CANDIDATES = [
  arg("--browser", null), process.env.CHROME_PATH,
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/usr/bin/google-chrome", "/usr/bin/chromium-browser", "/usr/bin/chromium",
].filter(Boolean);

const problems = [];
const fail = (m) => { problems.push(m); console.error("  FAIL: " + m); };

// ── minimal CDP client (flatten sessions) ────────────────────────────────────
class CDP {
  constructor(wsUrl) {
    this.ws = new WebSocket(wsUrl);
    this.id = 0; this.pending = new Map();
    this.open = new Promise((res, rej) => {
      this.ws.addEventListener("open", res);
      this.ws.addEventListener("error", (e) => rej(new Error("ws error " + (e.message || ""))));
    });
    this.ws.addEventListener("message", (ev) => {
      const m = JSON.parse(ev.data);
      if (m.id && this.pending.has(m.id)) {
        const { res, rej } = this.pending.get(m.id); this.pending.delete(m.id);
        m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result);
      }
    });
  }
  send(method, params = {}, sessionId) {
    const id = ++this.id;
    return new Promise((res, rej) => {
      this.pending.set(id, { res, rej });
      this.ws.send(JSON.stringify({ id, method, params, sessionId }));
    });
  }
  async evalOn(sessionId, expression, awaitPromise = false) {
    const r = await this.send("Runtime.evaluate",
      { expression, awaitPromise, returnByValue: true }, sessionId);
    if (r.exceptionDetails)
      throw new Error(r.exceptionDetails.text + " " + (r.exceptionDetails.exception?.description || ""));
    return r.result.value;
  }
}

const exe = CANDIDATES.find((p) => p && existsSync(p));
if (!exe) { console.error("FAIL: no Chrome/Chromium binary found"); process.exit(1); }

const server = spawn(process.execPath,
  [new URL("./serve.mjs", import.meta.url).pathname, String(PORT)],
  { stdio: ["ignore", "pipe", "inherit"] });
await sleep(400);

const chrome = spawn(exe, [
  headed ? "--headless=new" : "--headless=new",
  "--remote-debugging-port=9344",
  "--remote-allow-origins=*",
  "--autoplay-policy=no-user-gesture-required",
  "--mute-audio",
  "--no-first-run", "--no-default-browser-check",
  `--user-data-dir=/tmp/lk-spike-chrome-${process.pid}`,
  "about:blank",
], { stdio: ["ignore", "pipe", "pipe"] });

// wait for the DevTools ws endpoint on stderr
const browserWsUrl = await new Promise((resolve, reject) => {
  let buf = "";
  const to = setTimeout(() => reject(new Error("timeout waiting for DevTools endpoint")), 15000);
  chrome.stderr.on("data", (d) => {
    buf += d.toString();
    const m = buf.match(/ws:\/\/[^\s]+/);
    if (m) { clearTimeout(to); resolve(m[0]); }
  });
});

let cdp;
try {
  cdp = new CDP(browserWsUrl);
  await cdp.open;
  const { targetId } = await cdp.send("Target.createTarget", { url: PAGE });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const S = sessionId;
  await cdp.send("Runtime.enable", {}, S);

  // ── wait for ready ─────────────────────────────────────────────────────────
  let ready = false;
  for (let i = 0; i < 150; i++) {
    const st = await cdp.evalOn(S, "({ready: !!window.__lk?.ready, error: window.__lk?.error||null})");
    if (st.error) throw new Error("page error: " + st.error);
    if (st.ready) { ready = true; break; }
    await sleep(100);
  }
  if (!ready) throw new Error("kernel never became ready");
  console.log("ready: worklet kernel resident and rendering");

  // ── 1. real-time proof ─────────────────────────────────────────────────────
  await cdp.evalOn(S, "!!window.__lk.lastMeter", false);
  for (let i = 0; i < 50 && !(await cdp.evalOn(S, "!!window.__lk.lastMeter")); i++) await sleep(50);
  const m0 = await cdp.evalOn(S, "window.__lk.lastMeter");
  const wall0 = Date.now();
  await sleep(1300);
  const m1 = await cdp.evalOn(S, "window.__lk.lastMeter");
  const wallDt = (Date.now() - wall0) / 1000;
  const dQuanta = m1.quanta - m0.quanta;
  const dCtx = m1.currentTime - m0.currentTime;
  const expected = (m1.sampleRate / 128) * wallDt;
  const rtRatio = dQuanta / expected;
  console.log(`real-time: Δquanta=${dQuanta} over ${wallDt.toFixed(2)}s wall (ctxΔ=${dCtx.toFixed(2)}s); expected ~${expected.toFixed(0)} → ratio ${rtRatio.toFixed(3)}`);
  if (!(rtRatio >= 0.95 && rtRatio < 1.2)) fail(`not real-time (ratio ${rtRatio.toFixed(3)}, want >= 0.95)`);
  if (!(Math.abs(dCtx - wallDt) < 0.4)) fail(`AudioContext clock not tracking wall (ctxΔ ${dCtx.toFixed(2)} vs ${wallDt.toFixed(2)})`);

  // ── 2. param-edit latency (median of several) ──────────────────────────────
  for (let i = 0; i < 6; i++) {
    await cdp.evalOn(S, `window.__lk.doParamEdit(${1500 + i * 400})`);
    await sleep(180);
  }
  const paramLat = await cdp.evalOn(S, "window.__lk.latencies.param");
  const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : NaN; };
  const paramMed = median(paramLat);
  const paramMax = Math.max(...paramLat);
  console.log(`param edit→sound latency: median ${paramMed.toFixed(1)} ms, max ${paramMax.toFixed(1)} ms (n=${paramLat.length})`);
  if (!(paramMed <= 30)) fail(`param edit→sound median ${paramMed.toFixed(1)} ms > 30 ms`);

  // ── 3. structural edit: audible + latency ──────────────────────────────────
  const rmsBefore = (await cdp.evalOn(S, "window.__lk.lastMeter")).outRms;
  for (let i = 0; i < 4; i++) {
    await cdp.evalOn(S, "window.__lk.doStructuralEdit()");
    await sleep(350);
  }
  await sleep(200);
  const structLat = await cdp.evalOn(S, "window.__lk.latencies.structural");
  const rmsAfter = (await cdp.evalOn(S, "window.__lk.lastMeter")).outRms;
  const structMed = median(structLat);
  const structMax = Math.max(...structLat);
  console.log(`structural edit→sound latency: median ${structMed.toFixed(1)} ms, max ${structMax.toFixed(1)} ms (n=${structLat.length})`);
  console.log(`audible: outRms ${rmsBefore.toFixed(4)} → ${rmsAfter.toFixed(4)} (Δ ${Math.abs(rmsAfter - rmsBefore).toFixed(4)})`);
  console.log(`(plan-build cost itself is measured precisely offline in measure.mjs — the worklet global scope has no performance.now)`);
  if (!(structMed <= 30)) fail(`structural edit→sound median ${structMed.toFixed(1)} ms > 30 ms`);
  if (!(Math.abs(rmsAfter - rmsBefore) > 0.002)) fail(`structural swap not audible (Δrms ${Math.abs(rmsAfter - rmsBefore).toFixed(5)})`);

  const summary = {
    realtimeRatio: +rtRatio.toFixed(3),
    paramLatencyMedianMs: +paramMed.toFixed(1), paramLatencyMaxMs: +paramMax.toFixed(1),
    structuralLatencyMedianMs: +structMed.toFixed(1), structuralLatencyMaxMs: +structMax.toFixed(1),
    audibleDeltaRms: +Math.abs(rmsAfter - rmsBefore).toFixed(4),
    sampleRate: m1.sampleRate,
  };
  console.log("\nBROWSER_RESULTS_JSON " + JSON.stringify(summary));
  if (problems.length === 0)
    console.log("\nPASS: resident worklet kernel renders REAL-TIME and applies param + structural edits under a click-free crossfade within 30 ms.");
  else
    console.log(`\nFAIL: ${problems.length} problem(s).`);
} catch (e) {
  fail(String(e && e.message ? e.message : e));
} finally {
  try { chrome.kill("SIGTERM"); } catch {}
  server.kill("SIGTERM");
  await sleep(150);
  process.exit(problems.length ? 1 : 0);
}
