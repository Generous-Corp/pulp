// Headless-Chrome validation of the F2 compiled tier (f2-emitter.js).
//
// Proves, in a REAL AudioWorkletGlobalScope (system Chrome, no COOP/COEP, no
// user gesture), the parts of F2-S1 the offline harness cannot: that the
// emitter runs INSIDE the worklet (classic script, no deps), that the emitted
// module sync-compiles + instantiates against the resident kernel's f2_* libm
// bridge on the render thread, that the equal-power handoff engages both ways
// (interp → compiled on f2Compile, compiled → interp on any live edit), and
// that rendering stays real-time in the compiled tier.
//
// Usage: node f2-validate.mjs [--browser <path>]. Exit 0 = PASS.
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { existsSync } from "node:fs";

const arg = (flag, dflt) => {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
};
const PORT = 8795;
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
  async evalOn(sessionId, expression) {
    const r = await this.send("Runtime.evaluate", { expression, returnByValue: true }, sessionId);
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
  "--headless=new",
  "--remote-debugging-port=9345",
  "--remote-allow-origins=*",
  "--autoplay-policy=no-user-gesture-required",
  "--mute-audio",
  "--no-first-run", "--no-default-browser-check",
  `--user-data-dir=/tmp/lk-f2-chrome-${process.pid}`,
  "about:blank",
], { stdio: ["ignore", "pipe", "pipe"] });

const browserWsUrl = await new Promise((resolve, reject) => {
  let buf = "";
  const to = setTimeout(() => reject(new Error("timeout waiting for DevTools endpoint")), 15000);
  chrome.stderr.on("data", (d) => {
    buf += d.toString();
    const m = buf.match(/ws:\/\/[^\s]+/);
    if (m) { clearTimeout(to); resolve(m[0]); }
  });
});

const waitFor = async (cdp, S, expr, ms, what) => {
  for (let i = 0; i < Math.ceil(ms / 100); i++) {
    if (await cdp.evalOn(S, expr)) return true;
    await sleep(100);
  }
  throw new Error("timeout waiting for " + what);
};

let cdp;
try {
  cdp = new CDP(browserWsUrl);
  await cdp.open;
  const { targetId } = await cdp.send("Target.createTarget", { url: PAGE });
  const { sessionId: S } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  await cdp.send("Runtime.enable", {}, S);

  await waitFor(cdp, S, "!!window.__lk?.ready", 15000, "kernel ready");
  await waitFor(cdp, S, "!!window.__lk.lastMeter", 5000, "meter");
  console.log("ready: worklet kernel resident (interp tier)");

  // ── 1. compile the current graph in the worklet + handoff to compiled ──────
  await cdp.evalOn(S, "window.__lk.doF2Compile(50)");
  await waitFor(cdp, S, "window.__lk.f2?.status === 'armed'", 5000, "f2 armed");
  const f2msg = await cdp.evalOn(S, "window.__lk.f2");
  console.log(`f2 armed: emitted ${f2msg.moduleBytes} B module in-worklet (imports: ${(f2msg.imports || []).join(",") || "none"})`);
  await waitFor(cdp, S, "window.__lk.lastMeter?.mode === 'compiled'", 5000, "compiled tier");
  console.log("handoff: interp → compiled complete (equal-power fade)");

  // ── 2. real-time + audible in the compiled tier ────────────────────────────
  const m0 = await cdp.evalOn(S, "window.__lk.lastMeter");
  const wall0 = Date.now();
  await sleep(1300);
  const m1 = await cdp.evalOn(S, "window.__lk.lastMeter");
  const wallDt = (Date.now() - wall0) / 1000;
  const rtRatio = (m1.quanta - m0.quanta) / ((m1.sampleRate / 128) * wallDt);
  console.log(`real-time (compiled): Δquanta=${m1.quanta - m0.quanta} over ${wallDt.toFixed(2)}s → ratio ${rtRatio.toFixed(3)}; outRms=${m1.outRms.toFixed(4)}`);
  if (m1.mode !== "compiled") fail(`expected compiled tier, got ${m1.mode}`);
  if (!(rtRatio >= 0.95 && rtRatio < 1.2)) fail(`compiled tier not real-time (ratio ${rtRatio.toFixed(3)})`);
  if (!(m1.outRms > 0.001)) fail(`compiled tier silent (outRms ${m1.outRms})`);

  // ── 3. live edit falls back to the interpreter (dev tier) ──────────────────
  await cdp.evalOn(S, "window.__lk.doParamEdit(3200)");
  await waitFor(cdp, S, "window.__lk.lastMeter?.mode === 'interp'", 5000, "fallback to interp");
  console.log("fallback: live edit → compiled → interp fade-back complete");
  const lat = await cdp.evalOn(S, "window.__lk.latencies.param");
  if (!lat.length) fail("param edit not applied during fallback");

  // ── 4. recompile works after fallback ───────────────────────────────────────
  await cdp.evalOn(S, "window.__lk.doF2Compile(50)");
  await waitFor(cdp, S, "window.__lk.lastMeter?.mode === 'compiled'", 5000, "recompile");
  console.log("recompile: interp → compiled again");

  const summary = {
    compiledModuleBytes: f2msg.moduleBytes,
    realtimeRatioCompiled: +rtRatio.toFixed(3),
    outRmsCompiled: +m1.outRms.toFixed(4),
  };
  console.log("\nBROWSER_RESULTS_JSON " + JSON.stringify(summary));
  if (problems.length === 0)
    console.log("\nPASS: F2 emits + compiles in the worklet, hands off under the equal-power fade, renders real-time, and falls back to the interpreter on live edits.");
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
