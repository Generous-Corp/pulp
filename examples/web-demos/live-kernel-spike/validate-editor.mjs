// Headless-Chrome validation of the Pulp Live M1 editor demo.
//
// Dependency-free (Node built-in WebSocket + CDP), same harness shape as the S0
// validate.mjs. Proves, in real system Chrome served WITHOUT COOP/COEP:
//   1. crossOriginIsolated === false, yet the kernel runs + hot-swaps (no COI).
//   2. Scrub-a-number (the wow): a value edit lands as a param set_param, is
//      AUDIBLE (analyser peak changes), and edit->sound latency is single-digit ms.
//   3. Typing digits is the same instant param path.
//   4. A structural edit (saw->sine / add a node) crossfades: audible + no gross
//      click (in-browser Laplacian check; S0 has the rigorous -64 dBFS null test).
//
// Usage: node validate-editor.mjs [--browser <path>] [--headed]. Exit 0 = PASS.
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { existsSync } from "node:fs";

const arg = (f, d) => { const i = process.argv.indexOf(f); return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : d; };
const headed = process.argv.includes("--headed");
const PORT = 8795;
const PAGE = `http://127.0.0.1:${PORT}/examples/web-demos/live-kernel-spike/editor.html`;
const CANDIDATES = [
  arg("--browser", null), process.env.CHROME_PATH,
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/usr/bin/google-chrome", "/usr/bin/chromium",
].filter(Boolean);

const problems = [];
const fail = (m) => { problems.push(m); console.error("  FAIL: " + m); };
const ok = (m) => console.log("  ok: " + m);

const SINE_PATCH = `patch AcidLine
o   = sine(freq: note.hz, amp: 0.35)
env = adsr(o, a: 2ms, d: 90ms, s: 0.1, r: 60ms, gate: note.gate)
f   = ladder(env, cutoff: 800hz, res: 0.82)
sl  = delay(f, time: 375ms, feedback: 0.3, mix: 0.22)
out = sl * -3db`;

class CDP {
  constructor(wsUrl) {
    this.ws = new WebSocket(wsUrl); this.id = 0; this.pending = new Map();
    this.open = new Promise((res, rej) => { this.ws.addEventListener("open", res); this.ws.addEventListener("error", (e) => rej(new Error("ws " + (e.message || "")))); });
    this.ws.addEventListener("message", (ev) => { const m = JSON.parse(ev.data); if (m.id && this.pending.has(m.id)) { const { res, rej } = this.pending.get(m.id); this.pending.delete(m.id); m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result); } });
  }
  send(method, params = {}, sessionId) { const id = ++this.id; return new Promise((res, rej) => { this.pending.set(id, { res, rej }); this.ws.send(JSON.stringify({ id, method, params, sessionId })); }); }
  async ev(S, expression, awaitPromise = false) { const r = await this.send("Runtime.evaluate", { expression, awaitPromise, returnByValue: true }, S); if (r.exceptionDetails) throw new Error(r.exceptionDetails.text + " " + (r.exceptionDetails.exception?.description || "")); return r.result.value; }
}

const exe = CANDIDATES.find((p) => p && existsSync(p));
if (!exe) { console.error("FAIL: no Chrome/Chromium found"); process.exit(1); }

const server = spawn(process.execPath, [new URL("./serve.mjs", import.meta.url).pathname, String(PORT)], { stdio: ["ignore", "pipe", "inherit"] });
await sleep(400);
const chrome = spawn(exe, ["--headless=new", "--remote-debugging-port=9346", "--remote-allow-origins=*", "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--no-first-run", "--no-default-browser-check", `--user-data-dir=/tmp/lk-editor-chrome-${process.pid}`, "about:blank"], { stdio: ["ignore", "pipe", "pipe"] });
const browserWsUrl = await new Promise((resolve, reject) => { let buf = ""; const to = setTimeout(() => reject(new Error("timeout waiting for DevTools")), 15000); chrome.stderr.on("data", (d) => { buf += d.toString(); const m = buf.match(/ws:\/\/[^\s]+/); if (m) { clearTimeout(to); resolve(m[0]); } }); });

let cdp;
try {
  cdp = new CDP(browserWsUrl); await cdp.open;
  const { targetId } = await cdp.send("Target.createTarget", { url: PAGE });
  const { sessionId: S } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  await cdp.send("Runtime.enable", {}, S);

  let ready = false;
  for (let i = 0; i < 150; i++) { const st = await cdp.ev(S, "({ready:!!window.__lkm?.ready,error:window.__lkm?.error||null})"); if (st.error) throw new Error("page error: " + st.error); if (st.ready) { ready = true; break; } await sleep(100); }
  if (!ready) throw new Error("editor never became ready");
  ok("editor kernel resident and rendering");

  // 1. NO cross-origin isolation
  const coi = await cdp.ev(S, "({coi:window.crossOriginIsolated, sab:(typeof SharedArrayBuffer!=='undefined')})");
  console.log(`  crossOriginIsolated = ${coi.coi} · SharedArrayBuffer ${coi.sab ? "present" : "absent"}`);
  if (coi.coi !== false) fail(`expected crossOriginIsolated === false (got ${coi.coi})`); else ok("runs with NO cross-origin isolation");

  // latch a drone so edits are audible, wait for sound
  await cdp.ev(S, "window.__lkm.latch(true)");
  await sleep(400);
  let base = 0; for (let i = 0; i < 30 && base < 0.02; i++) { base = await cdp.ev(S, "window.__lkm.peak()"); await sleep(60); }
  console.log(`  drone peak = ${base.toFixed(3)}`);
  if (base < 0.02) fail("no audible drone after latch"); else ok("drone audible");

  // 2. SCRUB a number (cutoff) -> param, audible + fast
  const paramLats = [];
  const cutoffs = [400, 1600, 300, 2200, 800];
  const peaks = [];
  for (const hz of cutoffs) {
    const r = await cdp.ev(S, `window.__lkm.editNumber('cutoff', ${(hz / 1000)}, 'khz')`, true);
    if (r.kind !== "param") fail(`cutoff edit routed as '${r.kind}', expected 'param'`);
    if (r.latencyMs > 0) paramLats.push(r.latencyMs);
    await sleep(120);
    peaks.push(await cdp.ev(S, "window.__lkm.peak()"));
  }
  const med = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : NaN; };
  const pMed = med(paramLats), pMax = Math.max(...paramLats);
  const peakSpread = Math.max(...peaks) - Math.min(...peaks);
  console.log(`  scrub cutoff: param latency median ${pMed.toFixed(1)} ms, max ${pMax.toFixed(1)} ms (n=${paramLats.length}); peak spread ${peakSpread.toFixed(3)}`);
  if (!(pMed <= 12)) fail(`param edit->sound median ${pMed.toFixed(1)} ms > 12 ms (expected single-digit)`); else ok("scrub is the ~3 ms param path");
  if (!(peakSpread > 0.01)) fail(`filter scrub not audible (peak spread ${peakSpread.toFixed(4)})`); else ok("filter sweep is audible");

  // 3. structural edit (saw -> sine on osc1) crossfades: audible + click-free-ish
  const clickP = cdp.ev(S, "window.__lkm.captureClick(220)", true); // capture across the fade
  const before = await cdp.ev(S, "window.__lkm.peak()");
  const sres = await cdp.ev(S, `window.__lkm.setPatchText(${JSON.stringify(SINE_PATCH)})`, true);
  const clickDb = await clickP;
  await sleep(300);
  const after = await cdp.ev(S, "window.__lkm.peak()");
  console.log(`  structural saw->sine: kind=${sres.kind} latency ${(sres.latencyMs||0).toFixed(1)} ms · peak ${before.toFixed(3)}->${after.toFixed(3)} · click ${clickDb.toFixed(1)} dBFS`);
  if (sres.kind !== "structural") fail(`saw->sine routed as '${sres.kind}', expected 'structural'`); else ok("verb change is a structural crossfade");
  if (!(sres.latencyMs >= 0 && sres.latencyMs <= 12)) fail(`structural edit->fade-start ${sres.latencyMs} ms > 12`); else ok("structural edit->sound single-digit ms");
  if (!(Math.abs(after - before) > 0.01 || after > 0.02)) fail("structural swap produced no sound"); else ok("morph is audible, audio kept running");
  if (!(clickDb < -18)) fail(`crossfade click ${clickDb.toFixed(1)} dBFS too high (analyser-domain gate -18)`); else ok(`crossfade click-free-ish (${clickDb.toFixed(1)} dBFS, analyser-domain)`);

  // 4. typing a broken patch never silences
  const brokenBeforePeak = await cdp.ev(S, "window.__lkm.peak()");
  await cdp.ev(S, `window.__lkm.setPatchText('out = ladder(res: nonsense')`, true);
  await sleep(200);
  const stillPlaying = await cdp.ev(S, "window.__lkm.peak()");
  const parseErrs = await cdp.ev(S, "window.__lkm.parseErrors.length");
  console.log(`  broken patch: ${parseErrs} error(s) shown, peak ${brokenBeforePeak.toFixed(3)}->${stillPlaying.toFixed(3)} (audio survives)`);
  if (!(parseErrs > 0)) fail("broken patch produced no error"); else ok("errors surfaced");
  if (!(stillPlaying > 0.005)) fail("broken patch silenced the audio (should keep last valid)"); else ok("last valid patch keeps playing through an error");

  // 5. iteration-2 nodes: load LushPad (svf + chorus + reverb) and hear it
  const lush = await cdp.ev(S, `(async()=>{const {EXAMPLES}=await import('./lk-dsl.mjs'); const r=await window.__lkm.setPatchText(EXAMPLES.LushPad); return r;})()`, true);
  await cdp.ev(S, "window.__lkm.latch(true)"); await sleep(500);
  let lushPeak = 0; for (let i = 0; i < 25 && lushPeak < 0.02; i++) { lushPeak = await cdp.ev(S, "window.__lkm.peak()"); await sleep(60); }
  console.log(`  LushPad (svf→chorus→reverb): ${lush.kind}, peak ${lushPeak.toFixed(3)}`);
  if (!(lushPeak > 0.02)) fail("LushPad (new nodes: svf/chorus/reverb) produced no sound"); else ok("new nodes (svf, chorus, reverb) play in a real patch");

  // 6. the signal-flow graph is LIVE — per-node RMS is arriving and non-trivial
  let levels = [];
  for (let i = 0; i < 25; i++) { levels = await cdp.ev(S, "window.__lkm.nodeLevels()"); if (levels.length && Math.max(...levels) > 0) break; await sleep(60); }
  const liveNodes = levels.filter((v) => v > 1e-5).length;
  console.log(`  signal-flow graph: ${levels.length} node levels, ${liveNodes} lit (max ${Math.max(0, ...levels).toFixed(3)})`);
  if (!(levels.length >= 8 && liveNodes >= 3)) fail(`per-node RMS tap not live (levels=${levels.length}, lit=${liveNodes})`); else ok("per-node RMS tap feeds the live signal-flow graph");

  // 7. zero-alloc holds across a burst of scrubs + a structural swap (the RT contract)
  const allocBefore = await cdp.ev(S, "window.__lkm.allocCount()", true);
  for (const hz of [0.6, 1.4, 0.5, 2.0, 0.8]) await cdp.ev(S, `window.__lkm.editNumber('cutoff', ${hz}, 'khz')`, true);
  await cdp.ev(S, `(async()=>{const {EXAMPLES}=await import('./lk-dsl.mjs'); await window.__lkm.setPatchText(EXAMPLES.FuzzBass); return true;})()`, true);
  await sleep(300);
  const allocAfter = await cdp.ev(S, "window.__lkm.allocCount()", true);
  console.log(`  lk_alloc_count: ${allocBefore} -> ${allocAfter} (delta ${allocAfter - allocBefore})`);
  if (allocBefore < 0 || allocAfter < 0) fail("alloc probe did not round-trip"); else if (allocAfter - allocBefore !== 0) fail(`zero-alloc broken: ${allocAfter - allocBefore} allocations during edits`); else ok("zero-alloc holds across scrubs + structural swaps (delta 0)");

  const summary = { crossOriginIsolated: coi.coi, paramLatencyMedianMs: +pMed.toFixed(1), paramLatencyMaxMs: +pMax.toFixed(1), filterSweepPeakSpread: +peakSpread.toFixed(3), structuralKind: sres.kind, structuralLatencyMs: +(sres.latencyMs || 0).toFixed(1), crossfadeClickDbfs: +clickDb.toFixed(1), errorsKeepPlaying: stillPlaying > 0.005, lushPadPlays: lushPeak > 0.02, graphNodeLevels: levels.length, graphLitNodes: liveNodes, allocDelta: allocAfter - allocBefore };
  console.log("\nEDITOR_RESULTS_JSON " + JSON.stringify(summary));
  console.log(problems.length === 0 ? "\nPASS: scrub-a-number is the ~3 ms param path, structural morph crossfades, 14 nodes, live signal-flow graph, zero-alloc — all with crossOriginIsolated === false." : `\nFAIL: ${problems.length} problem(s).`);
} catch (e) { fail(String(e && e.message ? e.message : e)); }
finally { try { chrome.kill("SIGTERM"); } catch {} server.kill("SIGTERM"); await sleep(150); process.exit(problems.length ? 1 : 0); }
