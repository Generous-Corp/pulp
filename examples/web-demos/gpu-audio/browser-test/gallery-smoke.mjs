// Gallery smoke for the GPU demo page. No browser, no wasm, no build tree: it
// runs assemble-gallery.mjs against a STUB build dir (the script only copies the
// .wasm files, it never reads them) and asserts three things.
//
//   1. HONESTY: while the plugin -> page IR handoff is unwired
//      (GPU_IR_HANDOFF_WIRED === false in assemble-gallery.mjs), the page's GPU
//      engine can NEVER run — so the page must NOT be emitted at all. A page
//      titled "GPU engine", whose OG card says the convolution is running as a
//      WebGPU compute shader, that serves 100 % CPU to every visitor, is exactly
//      the capability lie this whole lane is supposed to refuse. When the handoff
//      lands, flip the constant and this file asserts the page instead (the
//      self-containment + copy checks below run in that mode).
//   2. Whichever way that goes, /super-convolver-gpu/ sits under the `/*` block in
//      cloudflare/_headers, so COOP/COEP/CORP apply to it — SharedArrayBuffer (the
//      worklet <-> worker rings) is unavailable without cross-origin isolation.
//   3. THE REGRESSION LOCK: /super-convolver/wam/ and /super-convolver/wclap/
//      are BYTE-IDENTICAL to the shipped demo. Their sha256 is pinned below; the
//      GPU engine ships as a NEW page precisely so the working demo cannot move.
//
// Usage: node gallery-smoke.mjs        (exit 0 = PASS)

import { createHash } from "node:crypto";
import { mkdtemp, mkdir, writeFile, readFile, rm } from "node:fs/promises";
import { existsSync } from "node:fs";
import { execFile } from "node:child_process";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";

const run = promisify(execFile);
const HERE = new URL(".", import.meta.url).pathname;
const REPO = resolve(HERE, "../../../..");
const CLOUDFLARE = resolve(REPO, "examples/web-demos/wclap-build/cloudflare");
const ASSEMBLE = join(CLOUDFLARE, "assemble-gallery.mjs");
const UI_SRC = resolve(REPO, "examples/web-demos/super-convolver-ui");
const BRIDGE_SRC = resolve(REPO, "examples/web-demos/gpu-audio/js");
const BRIDGE_FILES = ["gpu-bridge.mjs", "gpu-worker.mjs", "gpu-ring.mjs"];
const DSP_FILES = ["pulp-gpu-dsp.js", "pulp-gpu-dsp.wasm"];

// The 23 gallery targets + both SuperConvolver modules. Kept in step with
// SECTIONS in assemble-gallery.mjs; a rename there fails this smoke loudly
// (`missing wasm for ...`), which is the intent.
const TARGETS = [
  "MonoSynth", "SynthWithPresets", "Gain", "StateMemo", "MidiTranspose", "MpeSpreader",
  "MidiInspector", "SysexEcho",
  "Chorus", "CompressorExpander", "Delay", "Distortion", "Flanger", "Panning", "ParametricEq",
  "Phaser", "PingPong", "PitchShift", "RingMod", "Robotization", "Tremolo", "Vibrato", "Wah",
  "SuperConvolver", "SuperConvolverGpu",
];

// sha256 of the two shipped SuperConvolver pages, captured from the assembler
// BEFORE the GPU page existed (`git show HEAD:...assemble-gallery.mjs`), against
// the stub tree below.
//
// MODULO THE CACHE-BUST HASH, and that exception is load-bearing rather than a
// weakening: `?v=` is a content hash over the shared player AND pulp-ui.js, both
// of which these pages SHIP. pulp-ui.js gained one export (setGpuStatus) for the
// GPU page's status line, so the hash moves — and it MUST, or the two shipped
// pages would serve a stale cached pulp-ui.js next to a changed file. Everything
// else about them is pinned byte-for-byte.
const CACHEBUST = /\?v=[0-9a-f]{8}/g;
const normalize = (html) => html.replace(CACHEBUST, "?v=<player>");
// Re-pinned after rebasing onto main. These moved because MAIN moved them (the browser
// UI + Safari editor work changed both shipped pages), NOT because the GPU work touched
// them — verified by running origin/main's OWN assembler against the same stub inputs and
// getting byte-identical output. That check is the only thing that makes a re-pin
// honest: without it, "re-pin until green" quietly launders exactly the regression this
// pin exists to catch. Re-do it, do not just paste the new value.
const SHIPPED = {
  "super-convolver/wam/index.html":
    "0d5e576326bb5268a347ec121b59a4fe3bf67073843eb4c823cc949ccfd4f79d",
  "super-convolver/wclap/index.html":
    "7f4e977e0f6ed025067aee0cba535978da53022f31fdbd846e8c7b7c05304c97",
};

const steps = [];
const check = (name, pass, detail = "") => {
  steps.push({ name, pass, detail });
  console.log(`  ${pass ? "ok  " : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
};

const sha = (buf) => createHash("sha256").update(buf).digest("hex");

const work = await mkdtemp(join(tmpdir(), "pulp-gallery-smoke-"));
try {
  // Stub build trees. assemble-gallery copies the modules; it never parses them.
  const build = join(work, "build");
  const wamBuild = join(work, "wasm-build");
  const uiBuild = join(work, "build-webui");
  const gpuBuild = join(work, "build-gpu-dsp");
  const out = join(work, "public");
  for (const d of [build, wamBuild, uiBuild, gpuBuild]) await mkdir(d, { recursive: true });
  for (const t of TARGETS) await writeFile(join(build, `${t}.wasm`), `stub:${t}`);
  await writeFile(join(wamBuild, "SuperConvolverWorklet.js"), "// stub");
  for (const f of ["PulpSuperConvolverUi.js", "PulpSuperConvolverUi.wasm", "PulpSuperConvolverUi.data"])
    await writeFile(join(uiBuild, f), "// stub");
  for (const f of DSP_FILES) await writeFile(join(gpuBuild, f), "// stub");

  const bridgePresent = BRIDGE_FILES.every((f) => existsSync(join(BRIDGE_SRC, f)));

  const { stdout } = await run(process.execPath, [
    ASSEMBLE,
    "--build", build, "--wam-build", wamBuild, "--ui-build", uiBuild,
    "--gpu-build", gpuBuild, "--out", out,
    "--site-base", "https://pulp-wclap-demos.pages.dev",
  ], { cwd: CLOUDFLARE });
  console.log(stdout.split("\n").map((l) => "    " + l).join("\n"));

  // 1 — the page is emitted ONLY when it could actually run the GPU.
  const gpuDir = join(out, "super-convolver-gpu");
  const assembler = await readFile(ASSEMBLE, "utf8");
  const handoffWired = /const GPU_IR_HANDOFF_WIRED = true/.test(assembler);
  if (!handoffWired) {
    check("the GPU page is NOT emitted while its GPU engine could never run",
          !existsSync(gpuDir),
          "GPU_IR_HANDOFF_WIRED === false: nothing sets window.__scGpuIr, so the " +
          "handshake can only fail — the assembler must not ship a page whose title, " +
          "description and OG card claim a WebGPU shader that never dispatches");
    check("the root gallery does not link a page that was not written",
          !(await readFile(join(out, "index.html"), "utf8").catch(() => ""))
            .includes("./super-convolver-gpu/"),
          "no dangling link to the unemitted page");
  } else if (bridgePresent) {
    const want = [
      "index.html", "SuperConvolverGpu.wasm", "SuperConvolver.wasm",
      "PulpSuperConvolverUi.js", "PulpSuperConvolverUi.wasm", "PulpSuperConvolverUi.data",
      "pulp-ui.js", ...BRIDGE_FILES, ...DSP_FILES,
    ];
    const missing = want.filter((f) => !existsSync(join(gpuDir, f)));
    check("/super-convolver-gpu/ is emitted and self-contained", missing.length === 0,
          missing.length ? "missing: " + missing.join(", ") : `${want.length} files`);

    const html = await readFile(join(gpuDir, "index.html"), "utf8");
    check("the handshake runs BEFORE the adapter is created",
          html.indexOf("startGpuLane(") < html.indexOf("createWclapAdapter(ctx"),
          "probe() + startGpuLane() precede createWclapAdapter");
    check("the CPU module is loaded when the handshake fails",
          html.includes('gpuOk ? "./SuperConvolverGpu.wasm" : "./SuperConvolver.wasm"'),
          "dspUrl is chosen from the handshake result");
    check("the Engine toggle is HIDDEN, not disabled, when there is no working GPU lane",
          /const mountEngineToggle = \(\) => \{[\s\S]*?<select id="engine"/.test(html) &&
            !/disabled/.test(html),
          "the <select> only exists inside mountEngineToggle(); nothing is rendered disabled");
    check("the toggle is gated on the ring ACTUALLY being attached in the worklet",
          html.includes("adapter.descriptor.gpuLane"),
          "ringAttached = gpuOk && descriptor.gpuLane");

    // ── The IR handoff. This is what the page could not do before, and every one of
    //    these is a way it could look wired and not be.
    check("the page does NOT start the lane with an IR it cannot have",
          /startGpuLane\(\{[\s\S]*?ir: null/.test(html),
          "the plugin does not exist yet at handshake time — the IR arrives later, from it");
    check("the dead window.__scGpuIr seam is gone",
          !html.includes("__scGpuIr"),
          "nothing set it; a page reading it would silently never get an IR");
    check("the page forwards the PLUGIN's IR to the worker",
          html.includes("adapter.onIrChanged") && html.includes("lane.setIr("),
          "plugin (pulp_ir_* exports) → worklet → adapter.onIrChanged → lane.setIr() → worker");
    check("the Engine toggle appears only once the worker HAS that IR",
          /if \(lane\.setIr\(ir\)\) mountEngineToggle\(\)/.test(html),
          "mountEngineToggle() is called from the IR handler and nowhere else — a toggle " +
          "offered earlier would switch the audio to a GPU still convolving with the unit " +
          "impulse it was self-tested with, and the reverb would audibly vanish");
    // The DISCLAIMER must be present and no speed claim may be. The exact wording is allowed
    // to change — the copy got shorter because the long version pushed the plugin below the
    // fold on a phone — but "not faster than the CPU" is the one sentence that cannot be
    // edited out, so match the CLAIM, not a fixed string.
    const disclaimer = /not faster(?: than the CPU)?/i;
    check("the copy makes no speed claim",
          disclaimer.test(html) &&
            !/(faster than|speed-?up|[0-9]x faster)/i.test(html.replace(/not faster(?: than the CPU)?[a-z ]*/gi, "")),
          "a 'not faster than the CPU' disclaimer is present; no speed claim is");
    check("the status poll is a setInterval, not requestAnimationFrame",
          html.includes("setInterval(") && !/requestAnimationFrame\s*\(/.test(html),
          "rAF is throttled in a background tab — exactly when misses matter");
  } else {
    check("/super-convolver-gpu/ is emitted", !existsSync(gpuDir),
          `SKIPPED — the browser GPU bridge (${BRIDGE_FILES.join(", ")}) is not in the tree; ` +
          "the assembler correctly omitted the page");
  }

  // 2 — cross-origin isolation covers the new dir.
  const headers = await readFile(join(CLOUDFLARE, "_headers"), "utf8");
  const globalBlock = /^\/\*\s*$[\s\S]*?(?=^\S|\Z)/m.exec(headers)?.[0] || "";
  check("COOP/COEP/CORP apply to /super-convolver-gpu/ (the `/*` block)",
        /Cross-Origin-Opener-Policy: same-origin/.test(globalBlock) &&
          /Cross-Origin-Embedder-Policy: require-corp/.test(globalBlock),
        "SharedArrayBuffer needs crossOriginIsolated; `/*` matches every nested path");

  // 3 — the regression lock.
  for (const [rel, pinned] of Object.entries(SHIPPED)) {
    const html = await readFile(join(out, rel), "utf8");
    const got = sha(normalize(html));
    const clean = !/gpu|engine/i.test(html);
    if (pinned === "PIN_ME") {
      check(`${rel} — sha256 pin`, false,
            `UNPINNED. Capture it from the pre-change assembler and paste it in: ${got}`);
    } else {
      check(`${rel} is unchanged (modulo the cache-bust hash)`, got === pinned,
            got === pinned ? got.slice(0, 16) : `expected ${pinned.slice(0, 16)}…, got ${got.slice(0, 16)}…`);
    }
    check(`${rel} carries no GPU/Engine surface`, clean,
          clean ? "no gpu/engine token" : "the shipped page mentions gpu/engine");
  }
} catch (e) {
  check("assemble-gallery ran", false, String((e && e.message) || e));
} finally {
  await rm(work, { recursive: true, force: true });
}

if (steps.some((s) => !s.pass)) {
  console.error("FAIL: gallery smoke");
  process.exitCode = 1;
} else {
  console.log("PASS: the GPU page is emitted only when it can run, the isolation headers " +
              "cover it, and the two shipped pages did not move");
}
