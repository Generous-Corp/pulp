// Headless-Chrome pixel fixture for the Pulp UI wasm module.
//
// This is THE proof for the browser render stack: the Ganesh/WebGL2 surface, the
// SkFontMgr wiring behind text shaping, the requestAnimationFrame RenderLoop, and
// the browser WindowHost's pointer plumbing are all things no native unit test can
// reach. The fixture mounts the module against a MOCK HostAdapter (no audio, no
// DSP wasm), then asserts on real pixels and a real pointer gesture.
//
// It does NOT gate on an exact golden hash — GPU drivers vary. It gates on
// content, text presence, interaction, and the host->UI direction, and records a
// stable FNV-1a fingerprint so drift is observable.
//
// Prerequisite: build the module (see ../README.md).
//   node browser-test/validate.mjs --build ../build-webui --artifact /tmp/pulp-ui.png
// Exit 0 = PASS.

import http from "node:http";
import { readFile, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { extname, resolve } from "node:path";
import { chromium } from "playwright-core";

const HERE = new URL(".", import.meta.url).pathname;
const PORT = 8734;
const PAGE = `http://localhost:${PORT}/`;

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const buildDir = resolve(HERE, arg("--build", "../build-webui"));
const artifact = arg("--artifact", null);
const headed = process.argv.includes("--headed");

const BROWSERS = [
  arg("--browser", null),
  process.env.PLAYWRIGHT_CHROMIUM_PATH,
  process.env.CHROME_PATH,
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium-browser",
  "/usr/bin/chromium",
].filter(Boolean);

const MODULE_JS = resolve(buildDir, "PulpSuperConvolverUi.js");
if (!existsSync(MODULE_JS)) {
  console.error(`FAIL: no UI module at ${MODULE_JS} — build it first (see ../README.md).`);
  process.exit(1);
}

// The page: a mock HostAdapter + the real pulp-ui.js glue. The WebGL2 context is
// created HERE, before the module boots, with preserveDrawingBuffer so the
// drawing buffer survives compositing and can be read back deterministically.
// Emscripten's getContext("webgl2") on an already-initialized canvas returns
// this same context.
const INDEX_HTML = `<!doctype html>
<meta charset="utf-8">
<style>html,body{margin:0;background:#000}#pulp-ui{width:640px;height:360px;display:block}</style>
<canvas id="pulp-ui"></canvas>
<script type="module">
const canvas = document.getElementById("pulp-ui");
const gl = canvas.getContext("webgl2", { preserveDrawingBuffer: true, alpha: true, antialias: true });
window.__gl = gl;
window.__ready = false;
window.__error = null;
window.__events = [];

const PARAMS = [
  { id: 10, label: "Mix",  unit: "%",  minValue: 0, maxValue: 100, defaultValue: 35 },
  { id: 11, label: "Size", unit: "",   minValue: 0, maxValue: 1,   defaultValue: 0.5 },
  { id: 12, label: "Gain", unit: "dB", minValue: -24, maxValue: 24, defaultValue: 0 },
];

const adapter = {
  descriptor: { name: "SuperConvolver" },
  audioNode: null,
  getParameterInfo: async () => PARAMS,
  getParameterValue: async (id) => PARAMS.find((p) => p.id === id).defaultValue,
  setParameterValue: (id, value) => window.__events.push({ kind: "set", id, value }),
  beginGesture: (id) => window.__events.push({ kind: "begin", id }),
  endGesture: (id) => window.__events.push({ kind: "end", id }),
  onParamsChanged: null,
  onMidiOut: null,
  scheduleMidi() {}, sendSysex() {},
  getState: async () => new Uint8Array(), setState() {},
  createSecondary: async () => { throw new Error("not used"); },
  destroy() {},
};
window.__adapter = adapter;
window.__params = PARAMS;

try {
  const { mountPulpUi } = await import("/pulp-ui.js");
  window.__ui = await mountPulpUi(canvas, adapter, { moduleUrl: "/PulpSuperConvolverUi.js" });
  window.__ready = true;
} catch (e) {
  window.__error = String((e && e.stack) || e);
}
</script>`;

const MIME = {
  ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm",
  ".data": "application/octet-stream", ".html": "text/html",
};

const server = http.createServer(async (req, res) => {
  const path = decodeURIComponent(req.url.split("?")[0]);
  const head = {
    "cross-origin-opener-policy": "same-origin",
    "cross-origin-embedder-policy": "require-corp",
  };
  if (path === "/") {
    res.writeHead(200, { ...head, "content-type": "text/html" });
    return res.end(INDEX_HTML);
  }
  const name = path.replace(/^\//, "");
  if (/[/\\]/.test(name)) { res.writeHead(403); return res.end(); }
  const file = name === "pulp-ui.js" ? resolve(HERE, "..", name) : resolve(buildDir, name);
  try {
    const data = await readFile(file);
    res.writeHead(200, { ...head, "content-type": MIME[extname(file)] || "application/octet-stream" });
    res.end(data);
  } catch {
    console.log("  [404]", path);
    res.writeHead(404);
    res.end("not found: " + path);
  }
});
await new Promise((r) => server.listen(PORT, r));

const steps = [];
const check = (name, pass, detail = "") => {
  steps.push({ name, pass, detail });
  console.log(`  ${pass ? "ok  " : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
};

// The module's C ABI, listed in THREE places that must stay in sync: the
// EMSCRIPTEN_KEEPALIVE definitions in ui_entry.cpp, _PULP_WEBUI_EXPORTED_FUNCTIONS
// in tools/cmake/PulpWebUi.cmake, and here + pulp-ui.js. A rename that lands in
// only two of the three otherwise surfaces as an undefined-is-not-a-function deep
// inside a demo page.
const EXPECTED_EXPORTS = [
  "_malloc", "_free",
  "_pulp_ui_add_param", "_pulp_ui_init", "_pulp_ui_resize",
  "_pulp_ui_set_param", "_pulp_ui_get_param",
  "_pulp_ui_repaint", "_pulp_ui_gpu_available", "_pulp_ui_widget_rect",
  "_pulp_ui_capture_png", "_pulp_ui_shutdown",
];

let browser;
try {
  const exe = BROWSERS.find((p) => existsSync(p));
  if (!exe) throw new Error("no Chrome/Chromium binary found (set CHROME_PATH or --browser)");
  // On a headless Linux CI runner there is no GPU, so Chrome falls back to
  // SwiftShader for WebGL — and current Chrome refuses to do that without an
  // explicit opt-in, handing back a NULL WebGL2 context. This fixture's whole
  // point is that Skia's Ganesh backend renders on a real WebGL2 context, so a
  // null context must stay a hard failure (it does — see the context check
  // below); the flag only permits the software rasteriser that makes the check
  // runnable at all off a GPU host. It is a no-op on a machine with a real GPU.
  browser = await chromium.launch({
    executablePath: exe,
    headless: !headed,
    args: ["--enable-unsafe-swiftshader"],
  });
  const page = await browser.newPage({ viewport: { width: 900, height: 600 } });
  page.on("console", (m) => console.log("  [page]", m.text()));
  page.on("pageerror", (e) => console.log("  [pageerror]", e.message));
  await page.goto(PAGE, { waitUntil: "load" });

  await page.waitForFunction(() => window.__ready || window.__error, null, { timeout: 60000 });
  const bootError = await page.evaluate(() => window.__error);
  if (bootError) throw new Error("module failed to mount:\n" + bootError);

  // 0 — the C ABI. Cheap, and it catches the three-place drift before any pixel
  // check can turn it into a mystery.
  const missingExports = await page.evaluate((names) => {
    const M = window.__ui.module;
    return names.filter((n) => typeof M[n] !== "function");
  }, EXPECTED_EXPORTS);
  check("the module exports the full UI C ABI",
        missingExports.length === 0,
        missingExports.length ? "missing: " + missingExports.join(", ")
                              : `${EXPECTED_EXPORTS.length} exports`);

  // NO status-line assertions here, deliberately. The view tree used to carry a status
  // label that the page pushed GPU stats into at 10 Hz; it is gone. The page names the
  // engine in a <select> directly under the canvas — the same control that changes it —
  // and the live counters live in fixed-width DOM slots beside it. A label inside the
  // canvas whose text changed on a timer re-laid the view out on that timer, and at phone
  // width it sheared straight through the knob labels. What replaced it is DOM, and the
  // page fixtures cover it.

  // 1 — WebGL2 context. Fail loudly rather than silently rendering nothing.
  const glInfo = await page.evaluate(() => {
    const gl = window.__gl;
    if (!gl) return null;
    return { version: gl.getParameter(gl.VERSION), is2: typeof WebGL2RenderingContext !== "undefined" && gl instanceof WebGL2RenderingContext };
  });
  check("WebGL2 context created", !!glInfo && glInfo.is2, glInfo ? glInfo.version : "getContext('webgl2') returned null");
  if (!glInfo || !glInfo.is2) throw new Error("no WebGL2 context — the Ganesh surface cannot render");

  // Pixel probes. The GL readback proves the GPU surface actually painted; the
  // Skia raster capture (same canvas / TextShaper / font path, deterministic
  // top-left origin) is what the region checks and the PNG artifact use.
  const probe = await page.evaluate(async () => {
    const M = window.__ui.module;
    M._pulp_ui_repaint();

    const gl = window.__gl;
    const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight;
    const gpu = new Uint8Array(w * h * 4);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, gpu);

    const png = await capturePng(M);
    const raster = await decode(png);

    const rect = (slot, kind) => {
      const ptr = M._malloc(16);
      const ok = M._pulp_ui_widget_rect(slot, kind, ptr);
      const f = ok ? Array.from(M.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + 4)) : null;
      M._free(ptr);
      return f;
    };

    window.__probe = { gpu: { w, h, data: gpu }, raster, png };
    return {
      gpu: { w, h, distinct: distinctColors(gpu), nonBg: nonBackground(gpu), fingerprint: fnv1a(gpu, w, h) },
      raster: { w: raster.width, h: raster.height, distinct: distinctColors(raster.data), nonBg: nonBackground(raster.data) },
      knob: rect(0, 0),
      label: rect(0, 1),
      labelInk: raster.width ? regionInk(raster, rect(0, 1)) : 0,
      pngBase64: toBase64(png),
    };

    function toBase64(bytes) {
      let s = "";
      for (let i = 0; i < bytes.length; i += 0x8000)
        s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
      return btoa(s);
    }

    async function capturePng(M) {
      const pp = M._malloc(4), lp = M._malloc(4);
      const ok = M._pulp_ui_capture_png(pp, lp);
      if (!ok) { M._free(pp); M._free(lp); throw new Error("_pulp_ui_capture_png returned 0"); }
      const ptr = M.HEAP32[pp >> 2], len = M.HEAP32[lp >> 2];
      const bytes = M.HEAPU8.slice(ptr, ptr + len);
      M._free(ptr); M._free(pp); M._free(lp);
      return bytes;
    }
    async function decode(bytes) {
      const bmp = await createImageBitmap(new Blob([bytes], { type: "image/png" }));
      const c = new OffscreenCanvas(bmp.width, bmp.height);
      const ctx = c.getContext("2d");
      ctx.drawImage(bmp, 0, 0);
      const img = ctx.getImageData(0, 0, bmp.width, bmp.height);
      return { width: bmp.width, height: bmp.height, data: new Uint8Array(img.data.buffer) };
    }
    function distinctColors(px) {
      const seen = new Set();
      for (let i = 0; i < px.length; i += 4) {
        seen.add((px[i] << 16) | (px[i + 1] << 8) | px[i + 2]);
        if (seen.size > 4096) break;
      }
      return seen.size;
    }
    // The themed root fill is the modal color; anything else is drawn content.
    function nonBackground(px) {
      const hist = new Map();
      for (let i = 0; i < px.length; i += 4) {
        const k = (px[i] << 16) | (px[i + 1] << 8) | px[i + 2];
        hist.set(k, (hist.get(k) || 0) + 1);
      }
      let bg = 0, best = -1;
      for (const [k, n] of hist) if (n > best) { best = n; bg = k; }
      return (px.length / 4) - best;
    }
    function regionInk(img, r) {
      if (!r) return 0;
      const [x, y, w, h] = r.map(Math.round);
      const hist = new Map();
      const at = (px, py) => {
        const i = (py * img.width + px) * 4;
        return (img.data[i] << 16) | (img.data[i + 1] << 8) | img.data[i + 2];
      };
      for (let py = Math.max(0, y); py < Math.min(img.height, y + h); py++)
        for (let px = Math.max(0, x); px < Math.min(img.width, x + w); px++)
          hist.set(at(px, py), (hist.get(at(px, py)) || 0) + 1);
      let total = 0, best = 0;
      for (const n of hist.values()) { total += n; if (n > best) best = n; }
      return total - best;  // pixels that are not the region's dominant (background) color
    }
    // Same shape as HeadlessSurface::rgba_fingerprint: FNV-1a 64 over the
    // dimensions then the pixel bytes.
    function fnv1a(px, w, h) {
      const MASK = (1n << 64n) - 1n;
      const PRIME = 1099511628211n;
      let hash = 14695981039346656037n;
      const mix = (byte) => { hash = ((hash ^ BigInt(byte)) * PRIME) & MASK; };
      for (const b of new Uint8Array(new Uint32Array([w, h]).buffer)) mix(b);
      for (let i = 0; i < px.length; i++) mix(px[i]);
      return hash.toString(16).padStart(16, "0");
    }
  });

  // 2a — content floor: the GPU surface actually painted something.
  const gpuOk = probe.gpu.nonBg > 2000 && probe.gpu.distinct > 4;
  check("GPU canvas is not blank",
        gpuOk,
        `${probe.gpu.nonBg} non-background px, ${probe.gpu.distinct} distinct colors (${probe.gpu.w}x${probe.gpu.h})`);

  const rasterOk = probe.raster.nonBg > 2000 && probe.raster.distinct > 4;
  check("Skia raster capture is not blank",
        rasterOk,
        `${probe.raster.nonBg} non-background px, ${probe.raster.distinct} distinct colors`);

  // 2b — text presence. A null SkFontMgr / zero-width shaping leaves the label
  // region a flat fill; that is exactly what this catches.
  check("label region contains text ink",
        !!probe.label && probe.labelInk > 20,
        probe.label ? `${probe.labelInk} ink px in [${probe.label.map(Math.round).join(",")}]` : "no label rect");

  // 3 — a real pointer gesture on a real knob.
  const box = await page.evaluate(() => {
    const r = document.getElementById("pulp-ui").getBoundingClientRect();
    return { x: r.x, y: r.y };
  });
  const [kx, ky, kw, kh] = probe.knob || [0, 0, 0, 0];
  const cx = box.x + kx + kw / 2;
  const cy = box.y + ky + kh / 2;
  await page.evaluate(() => { window.__events.length = 0; });
  await page.mouse.move(cx, cy);
  await page.mouse.down();
  for (let i = 1; i <= 6; i++) await page.mouse.move(cx, cy - i * 6);  // drag up = increase
  await page.mouse.up();
  await page.waitForTimeout(120);

  const events = await page.evaluate(() => window.__events);
  const sets = events.filter((e) => e.kind === "set");
  const first = events[0], last = events[events.length - 1];
  const paramId = await page.evaluate(() => window.__params[0].id);
  const bracketed = !!first && first.kind === "begin" && first.id === paramId &&
                    !!last && last.kind === "end" && last.id === paramId;
  check("pointer drag emits a bracketed parameter edit",
        bracketed && sets.length > 0 && sets.every((e) => e.id === paramId),
        `${events.length} events: ${events.map((e) => e.kind).join(",")}`);

  const defaultValue = await page.evaluate(() => window.__params[0].defaultValue);
  const moved = sets.length > 0 && sets[sets.length - 1].value !== defaultValue;
  const monotone = sets.every((e, i) => i === 0 || e.value >= sets[i - 1].value);
  check("drag moves the value monotonically",
        moved && monotone,
        sets.length ? `${defaultValue} -> ${sets.map((e) => e.value.toFixed(2)).join(" -> ")}` : "no value events");

  // 4 — host -> UI round trip: the plugin changing its own value must repaint.
  const roundTrip = await page.evaluate(async () => {
    const M = window.__ui.module;
    const before = M._pulp_ui_get_param(0);
    const target = window.__params[0].maxValue;   // unambiguously different
    window.__adapter.onParamsChanged([target, 0.5, 0], window.__params);
    M._pulp_ui_repaint();
    const after = M._pulp_ui_get_param(0);

    const pp = M._malloc(4), lp = M._malloc(4);
    M._pulp_ui_capture_png(pp, lp);
    const ptr = M.HEAP32[pp >> 2], len = M.HEAP32[lp >> 2];
    const bytes = M.HEAPU8.slice(ptr, ptr + len);
    M._free(ptr); M._free(pp); M._free(lp);

    const bmp = await createImageBitmap(new Blob([bytes], { type: "image/png" }));
    const c = new OffscreenCanvas(bmp.width, bmp.height);
    const ctx = c.getContext("2d");
    ctx.drawImage(bmp, 0, 0);
    const now = new Uint8Array(ctx.getImageData(0, 0, bmp.width, bmp.height).data.buffer);

    const prev = window.__probe.raster.data;
    let diff = 0;
    for (let i = 0; i < Math.min(prev.length, now.length); i += 4) if (prev[i] !== now[i]) diff++;
    return { before, after, target, diff };
  });
  check("host -> UI: onParamsChanged repaints the knob",
        Math.abs(roundTrip.after - roundTrip.target) < 1e-3 && roundTrip.diff > 50,
        `value ${roundTrip.before} -> ${roundTrip.after} (target ${roundTrip.target}), ${roundTrip.diff} px changed`);

  // 5 — WebGL context LOSS and RESTORE. The browser can take the context away at
  // any time (a page over Chrome's live-context cap, a GPU-process crash, a
  // backgrounded mobile tab). Nothing in the GL API reports it, so without the
  // webglcontextlost/restored callbacks the surface would keep "rendering" into a
  // dead context and the UI would freeze silently while still claiming to be
  // available. WEBGL_lose_context revokes it for real — this is the actual event
  // path, not a simulation.
  const lossOk = await page.evaluate(async () => {
    const M = window.__ui.module;
    const ext = window.__gl.getExtension("WEBGL_lose_context");
    if (!ext) return { skipped: "WEBGL_lose_context unavailable" };

    const settle = () => new Promise((r) => setTimeout(r, 250));
    const before = M._pulp_ui_gpu_available();

    ext.loseContext();
    await settle();
    const whileLost = M._pulp_ui_gpu_available();
    // Must not crash / must not pretend to paint while the context is gone.
    M._pulp_ui_repaint();

    ext.restoreContext();
    await settle();
    const afterRestore = M._pulp_ui_gpu_available();

    M._pulp_ui_repaint();
    const gl = window.__gl;
    const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight;
    const px = new Uint8Array(w * h * 4);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    let nonZero = 0;
    for (let i = 0; i < px.length; i += 4) if (px[i] || px[i + 1] || px[i + 2]) nonZero++;
    return { before, whileLost, afterRestore, nonZero };
  });
  if (lossOk.skipped) {
    check("WebGL context loss/restore", true, "skipped — " + lossOk.skipped);
  } else {
    check("surface reports UNAVAILABLE while the WebGL context is lost",
          lossOk.before === 1 && lossOk.whileLost === 0,
          `available before=${lossOk.before} while-lost=${lossOk.whileLost}`);
    check("surface recovers (Ganesh rebuilt) and repaints after the context is restored",
          lossOk.afterRestore === 1 && lossOk.nonZero > 2000,
          `available=${lossOk.afterRestore}, ${lossOk.nonZero} painted px`);
  }

  // Gesture hygiene. A Pulp canvas is a control surface, not a document. Each of
  // these three shipped broken and was reported from a real page: a knob drag
  // selected page text, a drag panned the page, and focus() on pointerdown
  // scrolled the canvas into view — yanking the page out from under the pointer
  // mid-drag. Assert the styles AND the scroll behavior, because the styles are
  // what the browser consults before it decides a gesture is a scroll.
  const hygiene = await page.evaluate(async () => {
    // Whichever canvas the module bound to — the harness page and the deployed
    // demo page name it differently, and this check must hold for both.
    const c = document.querySelector("canvas#pulp-ui, canvas#pulp-ui-canvas") ||
              document.querySelector("canvas");
    const s = getComputedStyle(c);
    // The canvas must be OFF-SCREEN for this to mean anything: focus() only
    // scrolls when the element is not already in view. A spacer appended after
    // the canvas leaves it at the top of the page and the check passes even when
    // the bug is present — so push the canvas below the fold and scroll to top.
    const spacer = document.createElement("div");
    spacer.style.height = "3000px";
    c.parentNode.insertBefore(spacer, c);
    // The canvas is already focused from the earlier drag, and focus() on an
    // already-focused element never scrolls — so blur it, or this check cannot
    // fail even when the bug is present.
    if (document.activeElement === c) c.blur();
    window.scrollTo(0, 0);
    const scrollable = document.documentElement.scrollHeight > window.innerHeight;
    const r = c.getBoundingClientRect();
    c.dispatchEvent(new PointerEvent("pointerdown", {
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2,
      pointerId: 7, isPrimary: true, bubbles: true, cancelable: true,
    }));
    await new Promise((res) => requestAnimationFrame(res));
    const scrolledY = window.scrollY;
    // A drag across the canvas must not leave a text selection behind.
    const selLen = (window.getSelection()?.toString() || "").length;
    spacer.remove();
    return {
      touchAction: s.touchAction,
      userSelect: s.userSelect || s.webkitUserSelect,
      scrolledY,
      selLen,
      focused: document.activeElement === c,
    };
  });
  check("canvas surrenders touch-action (a drag must not pan the page)",
        hygiene.touchAction === "none", `touch-action: ${hygiene.touchAction}`);
  check("canvas is unselectable (a knob drag must not select page text)",
        hygiene.userSelect === "none", `user-select: ${hygiene.userSelect}`);
  check("pointerdown focuses the canvas WITHOUT scrolling the page",
        hygiene.focused && hygiene.scrolledY === 0,
        `focused=${hygiene.focused} scrollY=${hygiene.scrolledY} (must be 0)`);
  check("no text selected after a canvas gesture",
        hygiene.selLen === 0, `${hygiene.selLen} chars selected`);

  console.log(`  fingerprint(FNV-1a, GPU readback) = ${probe.gpu.fingerprint}`);

  if (artifact) {
    await writeFile(artifact, Buffer.from(probe.pngBase64, "base64"));
    console.log("  artifact:", artifact);
  }

  if (steps.some((s) => !s.pass)) throw new Error("one or more browser checks failed");
  console.log("PASS: Pulp's view tree renders and responds in a browser canvas (Ganesh/WebGL2)");
} catch (e) {
  console.error("FAIL: " + (e && e.message ? e.message : e));
  process.exitCode = 1;
} finally {
  if (browser) await browser.close();
  server.close();
}
