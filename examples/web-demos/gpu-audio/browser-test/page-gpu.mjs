// page-gpu.mjs — does the SHIPPED page actually run its GPU engine?
//
// validate-gpu.mjs proves the ENGINE. It does not, and cannot, prove the PAGE: it
// sidesteps the IR handoff entirely by measuring the impulse response on the CPU engine
// in one run and handing it to the worker in the next. A real visitor gets no such
// favour. Everything between the plugin's IR and the worker's kernel — the wasm exports,
// the worklet poll, the adapter latch, lane.setIr(), the worker's re-prepare — is
// UNCOVERED by that fixture, and every link in it can fail silently: the page would load,
// the CPU convolver would play, and the GPU engine would simply never appear.
//
// So this drives the ASSEMBLED page, in a real browser, exactly as a visitor does:
// start the audio, look for the Engine toggle, select GPU, and read the worker's own
// counters back out of the SharedArrayBuffer.
//
// The load-bearing assertion is `produced` ADVANCING. Not "the toggle exists" (it could
// be inert), not "no errors" (a silent CPU fallback is errorless by design), and not a
// cumulative "produced > 0" (that latches on the first block ever made and then reads
// GPU forever, including while a lost device misses every deadline). Blocks arriving
// NOW, on the page, after a click, is the claim — and it is only true if every link in
// the handoff chain is real.

import { chromium } from "playwright-core";
import { createServer } from "node:http";
import { existsSync, readFileSync } from "node:fs";
import { extname, join, resolve } from "node:path";

const arg = (k, d) => {
  const i = process.argv.indexOf(k);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : d;
};
const die = (m) => { console.error("error: " + m); process.exit(2); };

const SITE = resolve(arg("--site", ""));
if (!SITE || !existsSync(join(SITE, "super-convolver-gpu", "index.html")))
  die("--site <assembled public/ dir> must contain super-convolver-gpu/index.html");

const requireWebGpu = process.env.PULP_REQUIRE_WEBGPU === "1";
const BROWSERS = [
  process.env.CHROME_PATH,
  arg("--browser", ""),
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
].filter(Boolean);

const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".wasm": "application/wasm", ".data": "application/octet-stream",
  ".json": "application/json", ".png": "image/png", ".css": "text/css",
};

// The page needs crossOriginIsolated for its SharedArrayBuffer, which is the whole
// reason the WebCLAP lane can carry GPU audio at all. Serving it WITHOUT these headers
// would make the handshake fail for a reason that has nothing to do with the code under
// test, so this mirrors the real _headers file.
const server = createServer((req, res) => {
  const url = (req.url || "/").split("?")[0];
  let p = join(SITE, decodeURIComponent(url));
  if (p.endsWith("/")) p = join(p, "index.html");
  if (!existsSync(p)) { res.writeHead(404); res.end("not found"); return; }
  res.writeHead(200, {
    "Content-Type": MIME[extname(p)] || "application/octet-stream",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "cross-origin",
  });
  res.end(readFileSync(p));
});
await new Promise((r) => server.listen(0, r));
const BASE = `http://localhost:${server.address().port}`;

const steps = [];
const check = (name, pass, detail = "") => {
  steps.push({ name, pass });
  console.log(`  ${pass ? "ok  " : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
};
const note = (name, detail) => console.log(`  ..   ${name} — ${detail}`);

let browser;
try {
  const exe = BROWSERS.find((p) => existsSync(p));
  if (!exe) throw new Error("no Chrome/Chromium found (set CHROME_PATH or --browser)");
  browser = await chromium.launch({
    executablePath: exe,
    headless: true,
    args: ["--autoplay-policy=no-user-gesture-required"],
  });
  const page = await browser.newPage({ viewport: { width: 1100, height: 800 } });
  page.on("console", (m) => { if (/error|fail|gpu/i.test(m.text())) console.log("  [page]", m.text()); });
  page.on("pageerror", (e) => console.log("  [pageerror]", e.message));

  await page.goto(`${BASE}/super-convolver-gpu/`, { waitUntil: "load" });

  // The handshake runs at module scope and stashes its result. If the machine has no
  // WebGPU there is nothing to prove here and saying so is the honest outcome — but on a
  // lane that is SUPPOSED to have a GPU, a skip is a failure.
  await page.waitForFunction(() => window.__gpuProbe !== undefined, null, { timeout: 30000 });
  const probe = await page.evaluate(() => window.__gpuProbe);
  if (!probe || !probe.ok) {
    const reason = (probe && probe.reason) || "unknown";
    if (requireWebGpu) throw new Error(`PULP_REQUIRE_WEBGPU=1 and the page's handshake failed: ${reason}`);
    console.log(`SKIP: webgpu-unavailable (${reason})`);
    await browser.close(); server.close(); process.exit(0);
  }
  note("adapter (diagnostic, never proof)", JSON.stringify(probe.adapterInfo || {}));

  // Start the audio exactly as a visitor does. Everything downstream — the plugin
  // instantiating, activating, building its IR, publishing it — hangs off this click.
  await page.locator("#ov-start").click();

  // The Engine toggle DOES NOT EXIST until the worker has the plugin's kernel (the page
  // builds it from the IR handler and nowhere else). So waiting for it is not a UI
  // convenience — it IS the assertion that the whole handoff chain completed: exports →
  // worklet poll → adapter latch → lane.setIr() → worker re-prepare.
  let toggled = true;
  try {
    await page.waitForSelector("#engine", { timeout: 30000 });
  } catch {
    toggled = false;
  }
  check("the Engine toggle appeared — the plugin's IR reached the GPU worker", toggled,
        toggled ? "the page only builds this control from the IR handler"
                : "no #engine after 30 s: the IR never made it to the worker, so the page " +
                  "correctly refused to offer a GPU that would convolve with a unit impulse " +
                  "(i.e. play no reverb at all). The handoff is broken.");
  if (!toggled) throw new Error("the IR handoff did not complete on the page");

  // ── Engine=CPU means the GPU IS IDLE. ────────────────────────────────────────────────
  //
  // Not "its output is ignored" — IDLE. The page is still on CPU here (the default), and the
  // plugin is pushing every block across the xfer seam exactly as it does on GPU, so a worker
  // that convolves whatever it is handed will happily burn the GPU for audio nobody hears.
  // That is what it did: measured at ~100 queue submits per second with the select on CPU,
  // which is a pegged GPU meter and a drained battery for nothing. Read the worker's OWN
  // submit counter — the one number that cannot be talked out of the truth.
  const sampleStats = () => page.evaluate(() => {
    const st = window.__gpuStats;      // the worker's own counters, out of the SAB
    return st ? { produced: st.produced || 0, expired: st.expired || 0,
                  queueSubmits: st.queueSubmits || 0, primed: st.primed || 0 } : null;
  });

  const cpuBefore = await sampleStats();
  await page.evaluate(() => new Promise((r) => setTimeout(r, 2000)));
  const cpuAfter = await sampleStats();
  const cpuSubmits = (cpuAfter?.queueSubmits || 0) - (cpuBefore?.queueSubmits || 0);
  check("Engine=CPU does NO GPU work — not one dispatch",
        cpuSubmits === 0,
        cpuSubmits === 0
          ? "0 queue submits over 2 s on CPU (the worker drains the input ring into plain " +
            "memory and submits nothing)"
          : `${cpuSubmits} queue submits over 2 s while the engine is CPU — the GPU is ` +
            `convolving audio the user switched off`);

  // Flip to GPU and let it run. Read the worker's OWN counters from the SAB — not a
  // label the page prints, which could say anything.
  await page.selectOption("#engine", "1");
  await page.evaluate(() => new Promise((r) => setTimeout(r, 2500)));

  const sample = () => page.evaluate(() => {
    const st = window.__gpuStats;      // published by the page's own 10 Hz poll
    return st ? { produced: st.produced || 0, expired: st.expired || 0 } : null;
  });

  // The flip must also be CORRECT, not merely live. While the engine was CPU the worker did
  // no GPU work, so the convolver's frequency-domain delay line — where the reverb tail comes
  // from — went stale. Resuming with a stale line would smear a ghost of pre-flip audio under
  // the new material. The worker instead replays the input it buffered while idle, rebuilding
  // exactly the line it would have had. If this counter is 0, that replay did not happen and
  // the first ~IR-length after every flip is wrong.
  const flipped = await sampleStats();
  check("the flip to GPU PRIMED the convolver's delay line (no ghost of the CPU stretch)",
        (flipped?.primed || 0) > 0,
        `${flipped?.primed || 0} blocks replayed to rebuild the delay line`);

  // Sample a WINDOW, not a total. Cumulative counters cannot answer "is the GPU carrying
  // the audio", and on this page they are actively misleading: some blocks are SUPPOSED
  // to expire. The first L blocks have no wet (nothing was pushed L blocks before the
  // stream began), and the IR handoff deliberately DRAINS everything in flight so the
  // worker can re-prepare with the plugin's kernel — a re-prepare under a submitted block
  // would read back audio convolved with half of each IR. Those misses are the safety net
  // doing its job (the CPU convolver covers them), not the GPU failing.
  //
  // What must be true is that the STEADY STATE is the GPU's. So: let it settle, then
  // measure the deltas.
  const s1 = await sample();
  if (!s1) throw new Error("the page exposed no GPU stats to read");
  await page.evaluate(() => new Promise((r) => setTimeout(r, 2000)));
  const s2 = await sample();

  const produced = s2.produced - s1.produced;
  const expired = s2.expired - s1.expired;
  const total = produced + expired;

  // THE assertion. Blocks produced by the GPU worker, on the shipped page, right now.
  check("the GPU is producing audio blocks ON THE PAGE, right now", produced > 0,
        `produced advanced ${s1.produced} → ${s2.produced} (+${produced}) over 2 s` +
        (produced > 0 ? "" : " — the counter is not moving: the worklet is not consuming " +
         "the ring, or the worker has no kernel and is producing nothing"));

  // And that it is carrying essentially ALL of it once settled — the transient above is
  // over by now, so a steady stream of misses here would mean the GPU cannot hold real
  // time on this machine, which is a different (and real) failure than "it never ran".
  const missRate = total > 0 ? expired / total : 1;
  check("the GPU is holding real time — the CPU net is not quietly carrying the page",
        missRate < 0.05,
        `${expired}/${total} blocks missed in the steady-state window (${(missRate * 100).toFixed(1)}%)`);
  note("what a miss costs (diagnostic)",
       "nothing audible: a missed block is covered by the plugin's CPU convolver, which " +
       "runs primed the whole time. Only 'GPU only' removes that net.");

  const engineLabel = await page.evaluate(() => {
    const el = document.querySelector(".engine-off");
    return el ? el.textContent : null;
  });
  check("the page is not still showing a GPU-unavailable note", !engineLabel,
        engineLabel ? `still showing: ${engineLabel}` : "the note was replaced by the toggle");
} catch (e) {
  check("the page GPU run completed", false, String((e && e.message) || e));
} finally {
  if (browser) await browser.close();
  server.close();
}

if (steps.some((s) => !s.pass)) {
  console.error("FAIL: the shipped page did not run its GPU engine");
  process.exitCode = 1;
} else {
  console.log("PASS: the shipped page runs its WebGPU compute engine, fed by the plugin's own IR");
}
