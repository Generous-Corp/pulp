---
name: screenshot
description: Capture faithful PNGs of Pulp view trees / imported UIs headlessly. Covers render_to_png backends (Skia vs CoreGraphics), the image-compositing gotcha, capture_png from a live GPU host, and the --screenshot-backend validate flag. Use whenever you render a UI to a PNG to eyeball or montage it.
---

# Screenshotting Pulp UIs

Pulp has two headless capture surfaces plus the live-host capture. Picking the
wrong one wastes time on renders that look broken but aren't.

## Default to `capture_view` / `--backend auto` — it picks the backend for you

Don't hand-pick a backend unless you have a reason. `capture_view(root, w, h,
scale)` (`screenshot.hpp`) and the CLI default `pulp-screenshot --backend auto`
inspect the view tree and do the right thing, and **never return a silent blank**:

| The view tree… | …gets | Why |
|---|---|---|
| has a `contains_native_overlay()` subtree (a WebView / native NSView) | the overlay's **in-process snapshot** (`View::capture_native_overlay_png` → e.g. WKWebView `takeSnapshot`); **refused** with a reason only if no snapshot is available | A WebKit/native overlay is composited by the OS, NOT painted into the Pulp canvas — headless raster can't see it, but a backend that exposes an in-process snapshot can still be captured |
| has a `requires_gpu_host()` view (GPU content, custom SkSL) | the **`gpu`** path (`render_to_png_gpu`, offscreen Dawn+Skia via `HeadlessSurface`) | CPU raster does NOT render GPU-required views correctly — they come out blank/wrong |
| neither | CPU raster (`skia` when Skia is built, else the platform `default_backend` / a registered provider) | fast, faithful for vector + image widgets |

`capture_view` returns `{png, ok, used, reason}`; a blank/essentially-empty frame
sets `ok=false`. The CLI exits 3 (not a saved blank) on refuse/blank. This is the
"always-capturable" contract — if a UI didn't paint, you get told, not a blank PNG.

**Wrappers must opt in.** A WebView is only captured/refused correctly when the
owning Pulp `View` actually sets `set_contains_native_overlay(true)` and overrides
`capture_native_overlay_png()` to forward `WebViewPanel::snapshot_png()`. Without
that flag, `auto` rasters the (blank) overlay area silently — set it on any
wrapper that attaches a native child via `attach_native_child_view` (see
`examples/webview-plugin` `WebViewEditorPane`).

**Build gating.** The `gpu` path and `has_gpu_capture()` are compiled in only when
Skia is present (`PULP_VIEW_HAS_GPU_CAPTURE`, gated on `PULP_HAS_SKIA` — not merely
on the `pulp-render` target, whose `HeadlessSurface::create()` is a null stub in a
no-Skia build). In a no-Skia build the auto raster fallback is `default_backend`
(so a host-registered `ScreenshotProvider` handles it), not a forced `skia` that
would return empty bytes.

**When to override:** force `--backend gpu` to capture a GPU view that isn't
flagged `requires_gpu_host()`; use `skia`/`coregraphics` for the explicit cases
below. The image-compositing rule still applies to the raster backends.

## The image rule (raster backends): use **Skia** for anything with images

`render_to_png(root, w, h, scale, backend)`
(`core/view/include/pulp/view/screenshot.hpp`) takes a `ScreenshotBackend`:

| Backend | Composites file-backed images? | Use when |
|---------|-------------------------------|----------|
| `skia` | **Yes** | Default for anything real — designs with assets (Figma/Pencil imports), icons, photos; the fidelity reference |
| `coregraphics` (macOS default of `default_backend`) | **Yes, as of #6223** | Vector-only UIs, or when you specifically want the CG raster path |
| `default_backend` | macOS → CoreGraphics, else provider | Fine for images now; still prefer `skia` for import fidelity |

**"Skia is the fidelity reference" is about IMAGES, not about everything.**
The rule above exists because CG could not composite file-backed images — it is
not a general statement that the GPU path is more correct. Conic gradients were
the counter-example: Skia's sweep shader clamps angles outside its
`[start, end]` window instead of wrapping, so passing the CSS rotation as the
window's start left a flat wedge the size of that rotation, and the default
`from 0deg` lost a quarter of the circle. CoreGraphics software-rasterises the
sweep and wraps its angle correctly, so it never had the defect — a CG
screenshot of a conic was the more faithful one. Fixed by rotating the shader
instead of shifting its window, so the two agree again. The durable lesson:
when two backends disagree, find out WHICH is wrong before assuming; a
per-backend gap is a claim about one primitive, not a ranking.

**History (the trap — fixed #6223 S34):** `ImageView::paint` decodes images
on-paint via the canvas's `draw_image_from_file` / `measure_image_from_file`
primitive. `SkiaCanvas` always implemented it; the **CoreGraphics canvas did
not**, so every `ImageView` on the CG path fell back to drawing its **filename
as placeholder text** — an asset-rich import rendered as empty boxes with
`*.png` strings scattered across it. That looked like "missing images / broken
importer," but the import was fine — it was the backend. `#6223 S34` wired the
existing ImageIO decoder (`cg_decode_image_from_path_or_data`) into
`CoreGraphicsCanvas::draw_image_from_file{,_rect}` /
`draw_image_from_data{,_rect}` / `measure_image_from_file`, so **CG now
composites file-backed images** (right-side-up; the decoded CGImage is drawn
straight into the flipped canvas CTM — no counter-flip, unlike the
bottom-up conic-gradient bitmap). `Canvas::supports_image_draw()` (default
false; `SkiaCanvas` + `CoreGraphicsCanvas` return true) is the capability query
headless tooling can consult to warn instead of rendering unfaithfully.

**Still prefer Skia for import fidelity.** CG compositing works, but its raster
of gradients / anti-aliasing / sub-pixel placement differs from the live GPU
(Skia/Graphite) compositor an import ultimately runs on. Skia raster matches
that path far more closely, so it stays the fidelity reference for
`pulp import-design --validate` and montage comparisons. Behaviorally, expect
old CoreGraphics screenshot baselines of asset-bearing views to **shift** now
that images actually render (filename text → real pixels).

Confirmed 2026-06-02 on the ELYSIUM Figma import (pre-#6223): CoreGraphics →
empty vessel boxes + filename text; Skia → the gradient beakers/knobs/curves
all composite and the montage matches the Figma reference. Post-#6223 both
backends composite the images; Skia still wins on gradient/AA fidelity.

## Scoring an imported capture? Set `PULP_SHOT_NO_RECONCILE=1`

`pulp-screenshot` reconciles oversize absolutely-positioned descendants to the
capture viewport (`reconcile_oversize_absolute_subtree`). That exists so
runtime-imported React trees with a hardcoded oversize container still land
inside the frame, and it is the right default — leave it on for ordinary
captures.

It is **wrong for a faithful-capture import**, whose backdrop is exactly the
shape the clamp targets: `position:absolute`, a literal width (e.g. 1280px in a
920px root), no opposite-edge anchor, carrying bound controls positioned against
it. The clamp rescales the artwork out from under those controls. Nothing
errors; you simply score a different image than the one on disk, and the error
runs in **both** directions — it flatters a broken panel and crushes a correct
one, so it cannot even be corrected for after the fact.

So any comparison of a rendered panel against a reference render must set the
opt-out, or it is measuring the clamp:

```bash
PULP_SHOT_NO_RECONCILE=1 pulp-screenshot --script build.ui.js --backend skia …
```

Unset, empty, `0` and `false` all mean "reconcile", so a variable left exported
as `0` cannot silently disable reconciliation for every capture on the machine.

## Imported-design validation

`pulp import-design --validate --reference <png> --diff <png> [--render-size WxH]`
renders the generated JS and compares to a reference. As of 2026-06-02 it
defaults to `--screenshot-backend skia` (faithful). Only pass
`--screenshot-backend coregraphics` deliberately. A CoreGraphics validate of an
asset-heavy design is **not** a valid fidelity check.

```bash
pulp import-design --from figma-plugin --file scene.pulp.json --output ui.js \
  --validate --reference figma-ref.png --render-size 1146x746 --diff diff.png
# (skia is the default; add --screenshot-backend coregraphics only to escape-hatch)
```

**Pixel-% is a weak gate.** `compare_screenshot_files` similarity is exact-pixel
and very sensitive to gradients, anti-aliasing, sub-pixel placement, and
background differences — a visually faithful import can report a low % on a
gradient-heavy design. Treat the % as a smoke signal; **eyeball the montage**
(reference | render) for structure + assets.

**Do not hand-roll the comparison — the repo already ships these. Reach for
them before writing any PIL:**

| Need | Tool |
|---|---|
| Labeled N-panel montage | `python3 tools/import-design/montage.py --out cmp.png ...` |
| Per-widget fidelity audit vs a Figma source | `python3 tools/import-design/fidelity_diff.py --render r.png --scene scene.pulp.json --assets-dir DIR --frame-reference src.png` |
| Side-by-side + heatmap + worst offending regions | `python3 tools/scripts/figma_import_diff.py` |
| Masked per-region diff against a reference | `python3 tools/import-validation/diff_against_reference_regions.py` |
| Re-import regression vs a golden | `python3 tools/import-validation/golden_regression.py` |

This section used to end "Build montages with PIL." An agent comparing an
imported design followed that line, hand-rolled crop scripts, and never found
`fidelity_diff.py` — which was documented, but a thousand lines deep in a
different skill. Guidance that tells you to build what already exists is worse
than silence, so the invocation lives here rather than a pointer to go read
another file.

The full registry of these tools is **`docs/status/tools.yaml`**, whose digest
is generated into CLAUDE.md so it is always in context, and a PostToolUse hook
(`hooks/scripts/tool-registry-reminder.sh`) names the right tool if you start
writing PIL anyway. Three deliveries for one lesson: burial is what caused the
incident, so the fix is repetition at the moment of need.

`compare_screenshots` / `compare_screenshot_files` decode PNGs with
CoreGraphics on Apple and Skia on non-Apple builds when `PULP_HAS_SKIA=1`.
In a non-Apple no-Skia build, comparisons remain unavailable (`valid=false`,
empty diff/crop output) because there is no PNG decoder; treat that as a
missing comparison backend, not proof that the rendered PNG was empty.

## Capture options at a glance

For a bounded Renderer3D diagnostic, use the canonical
`renderer3d.hardcoded-cube.v1` recipe rather than substituting an arbitrary
screenshot:

```bash
mkdir -p "$PWD/artifacts/gpu/cube"
pulp gpu probe --recipe renderer3d.hardcoded-cube.v1 \
  --artifacts "$PWD/artifacts/gpu/cube" --json
```

The recipe drives native Dawn/WebGPU rendering and readback, then checks a
deterministic cube fingerprint on its declared backend or a portable structural
content oracle elsewhere. A pass proves only that bounded recipe on the
reported adapter; it is not cross-backend pixel identity, visible-window or
compositor evidence, screenshot fidelity, input behavior, or product-scene
coverage. Use the capture surfaces below for those separate claims.

Exit 0 is a completed pass, exit 1 is a completed content/readback failure, and
exit 2 is unavailable or unverified evidence, never a pass. To verify the
oracle rather than the renderer, rerun with `--negative-control`: GPU work and
readback must still complete while the planted content mutation produces a
typed failure. A nonblank artifact alone is insufficient; trust the recipe's
bounded evidence/result contract and adapter disclosure.

- **`render_to_png` / `render_to_file`** (`screenshot.hpp`) — headless raster of
  a `View` tree, no window. macOS/iOS have native backends; Linux/Windows use
  the built-in Skia raster path when `PULP_HAS_SKIA=1`, otherwise they need a
  host-registered provider via `set_screenshot_provider()` (else
  empty/"unsupported", not a silent blank — #299). Probe
  `has_screenshot_provider()`.
- **`WindowHost::capture_png()`** — reads the rendered frame from a live GPU host
  (`MacGpuWindowHost`). The most faithful (real paint path, GPU). Drive it from
  the design-tool example's `--no-show-window --automation-before <png>`
  offscreen path, or any host that exposes capture. NOTE: the design-tool's
  `--script` expects its own entry-module shape — a raw generated `ui.js` from
  `import-design` does not load that way (throws). Prefer `--validate` with the
  Skia backend for an imported `ui.js`.
- **Live remote capture** — use the canonical broker/control capture
  capability. The legacy `pulp inspect screenshot` caller was retired and must
  not be recreated as a fallback.
- **`pulp::view::render_to_file`** in tests — headless view-tree PNGs in CI.
- **`pulp::view::render_to_rgba`** (`screenshot.hpp`) — raw-pixel sibling of
  `render_to_png`. Returns the decoded **RGBA8** buffer (R,G,B,A byte order,
  premultiplied alpha, sRGB, top-to-bottom, stride `*out_width*4`) + the pixel
  dims, instead of PNG bytes — for callers that composite/upload the frame
  themselves (e.g. the foreign-host embed SDK's offscreen mode) and don't want a
  PNG encode+decode round-trip. **macOS-only** (forces the Skia raster path,
  which is endianness-independent; the non-Apple stub returns empty — the
  registered `ScreenshotProvider` is PNG-only). The internal `render_to_png_skia`
  already holds these pixels before encoding; this just exposes them. Note
  `AssetManager::decode_png` does NOT actually decode (it stores raw PNG bytes +
  parses IHDR), so you cannot get RGBA by round-tripping a PNG through it — use
  `render_to_rgba` for raw pixels.

## A standalone `--screenshot` run opens NO audio device

`StandaloneApp::start()` skips the audio backend entirely for a screenshot-only
launch: no `AudioSystem`, no device, no render callback, and no hardware MIDI
(nothing drains it without the callback). That is what makes the Standalone
format capturable on a shared or unattended machine — before it, every capture
opened a CoreAudio device, so the format could only be verified with a human at
the desk.

Consequences when you read such a capture:

- **Meters, scopes and any live-signal UI read zero.** That is the mode, not a
  broken UI. To capture a UI that must show real signal, set
  `StandaloneConfig::screenshot_keeps_audio` or export
  `PULP_SCREENSHOT_KEEP_AUDIO=1`.
- **The Settings tab's device lists are empty** — there is no audio system to
  enumerate. Capture with the keep-audio opt-in if the device picker is the
  subject of the shot.
- **Asking for a live readout in the same run keeps audio on**, because those
  readouts are produced BY the render callback: `--audio-probe-json`,
  `--audio-scope-json`, `--audio-capture-wav`, `--audio-capture-rolling`.

Verify a capture really stayed silent by process state, not by listening: while
the app is alive, `lsof -p <pid>` must not map
`/System/Library/Components/CoreAudio.component/…/CoreAudio`, and
`sample <pid> 1` must show no `com.apple.audio.IOThread.client` thread running
`HALC_ProxyIOContext::IOWorkLoop()`. Both appear when a device IS open, so
their absence is a two-state result rather than a hopeful one.

## Render size: use the design's true root, not the source bbox

`--render-size WxH` must match the imported design's **root frame** size, not
the source tool's reported node bounding box. A Figma node's screenshot bbox
includes page margin / shadow bleed around the frame (e.g. ELYSIUM: node bbox
1146×746, but the actual root "VST Style" frame is 1000×600). Render at the
bbox and the design lays out at top-left with the host's dark background
filling the extra pixels — looks like a "wrong window size." Read the root
size from `scene.pulp.json` `root.style.width/height` (or the generated
`setSize('root', …)`), and render at that. When in doubt, render at the root
size — the result fills the canvas and matches the design's own proportions.

## Headless GPU capture: prefer offscreen; Standalone live-host capture is hardened

The macOS GPU host normally drives visible windows per-vsync with
`CVDisplayLink`. An `initially_hidden` window may receive no display-link ticks
in an unattended WindowServer session, so Pulp switches that host to a 60 Hz
common-run-loop timer which invokes the same gated frame callback. This keeps
Standalone Inspector publication, frame-delayed `--screenshot=PATH` capture,
and teardown deterministic without showing the window. `show()` moves the host
back to its real display link. Keep the Standalone subprocess lifecycle tests
green when changing either path; they prove discovery, compositor capture,
application/window close drainage, off-mode behavior, and scripted UI state.

For a **headless render-only** capture of a GPU-rendered view, still prefer the
offscreen surface —
`render_to_file(root, w, h, path, scale, ScreenshotBackend::gpu)` /
`render_to_png_gpu` (Dawn+Skia `HeadlessSurface`, no window, no display link).
It renders the same tree through the real GPU stack and tears down cleanly, so
it has fewer lifecycle dependencies. Use the live Standalone `--screenshot`
path when the proof must include the actual host, Inspector, native overlays,
or compositor/back-buffer capture.

`examples/PulpTempoSampler` is the worked example of all three:
`pulp-tempo-sampler-shot OUT.png` (CPU raster, default), `… OUT.png --gpu`
(offscreen GPU, headless), and the standalone
`PulpTempoSampler … --screenshot=OUT.png` (live host lifecycle and capture).

## Gotchas

- **`render_to_rgba` is Skia-only, and without Skia it returns an EMPTY buffer
  rather than failing loudly.** The CoreGraphics path on macOS is PNG-only, so a
  build configured without Skia has no raw-pixel producer at all — every pixel
  probe reads back nothing no matter what the code under test does. This reads
  as a pile of unrelated feature failures: a UBSan lane once reported nine reds
  across box-shadow, inset shadow, mix-blend-mode, oklab and backdrop-filter,
  all from this one cause. The tell is that the suite's own control ("a plain
  fill actually reaches the buffer") is among the failures — when the control
  fails, the environment is the suspect, not the features.

  Ask `pulp::view::raw_rgba_render_available()` (in `screenshot.hpp`) and `SKIP`
  when it is false. It answers for the BUILD, mirroring the `#ifdef
  PULP_HAS_SKIA` that selects the backend. **Never skip on an empty result** —
  a build that CAN rasterize and produced nothing is a real defect, and keying
  the skip on the result would convert exactly that regression into a green run.
  Note the same trap applies to `render_to_png(..., ScreenshotBackend::skia)`,
  which is a named-backend request and likewise yields nothing without Skia.

  A skip has its own failure mode: a guard that wrongly reports "unavailable"
  greens a whole file without running any of it. Keep one case that never skips
  and asserts the guard matches reality in both directions.

- **Absolute-positioned leaf views need `preferred_width`/`preferred_height`,
  not just `dim_width`.** `yoga_layout.cpp` applies an explicit px size from
  `FlexStyle::preferred_width/height`; `dim_width = {w, px}` only reaches Yoga
  after `resolve_dimensions()` runs, which a bare `layout_children()` pass
  (e.g. the screenshot path's `paint_root`) does NOT call for absolute leaves.
  `Label`s survive on their text measure function, but a measure-less leaf like
  `WaveformEditor` collapses to 0×0 and `paint()` early-returns on
  `local_bounds().is_empty()` → blank. Set `v.flex().preferred_width = w;
  v.flex().preferred_height = h;` directly when placing such a child; a
  post-layout `set_bounds()` is futile because any later `layout_children()`
  re-collapses it.
- A fresh GPU-less build silently returns the CPU host on macOS
  (`PULP_HAS_SKIA` FALSE → `MacWindowHost`, not `MacGpuWindowHost`). Verify the
  binary contains `MacGpuWindowHost` before trusting a live capture (see the
  `import-design` skill's GPU-host gotcha).
- Don't call a CoreGraphics render of an asset-rich design the import result —
  re-render with Skia first. Post-#6223 CG does composite the images, but its
  gradient/AA raster still differs from the live GPU compositor an import runs
  on; Skia is the fidelity reference.
- **A non-empty PNG is not a passing render.** `ScreenshotStats::passes_content_floor`
  (`core/view/include/pulp/view/screenshot_compare.hpp`) is the oracle that catches
  the blank/near-blank-frame bug a raw "file written" check misses: it gates on a
  unique-color floor, a luminance-stddev floor, and non-background + opaque coverage
  floors. Assert it (not just `!png.empty()`) when validating a GPU capture, and
  pump enough settled frames first — a single unsettled frame can under-count
  content. The design-import screenshot-parity test asserts
  `REQUIRE_FALSE(passes_content_floor())` on a stable flat capture to prove the
  oracle actually rejects empties.

## Vision probe: prove image input works before reading a screenshot into context

A screenshot is only useful for review if the model can actually SEE it. Some
models/harnesses silently drop image input — the render succeeds, the bytes are
handed over, and the model then hallucinates a description of a picture it never
received. Before you read ANY capture into context for a visual judgment, run a
one-shot **vision probe**: hand the model a tiny, known committed PNG (e.g. one of
the small rendered fixtures under `test/fixtures/import-fidelity/assets/`, whose
content you have confirmed) and ask it to return exactly `VISION_OK` or
`VISION_UNSUPPORTED` based on a describable feature of that image — not "did an
image arrive" but "what does the shape look like".

- On `VISION_OK`: proceed to read real captures for visual review.
- On `VISION_UNSUPPORTED` (or any answer that does not match the known image):
  do NOT read screenshots into context and do NOT describe them. Save the
  captures to disk, report only their PATHS, and disclose explicitly: "visual
  review skipped — image input unavailable in this model/harness." Non-visual
  checks (logs, `passes_content_floor` on the raw bytes, `pulp design
  lint-adherence`) still run and still gate.

This degrades honestly across models instead of emitting confident fiction about
an unseen image.

## Verdict contract: one review artifact per pass, `done` or `needs_work`

When a verification pass reviews a render (see the read-only verifier in the
`import-design` skill), its final output is a verdict, never prose: exactly
`done`, or `needs_work: <root cause>` naming the constraint/token/log defect —
not the pixel symptom. A "looks good" with no verdict is a failure mode; so is a
reviewer that edits while reviewing. Keep review read-only and let the main agent
apply fixes, then re-run the verifier.

## Offscreen capture suspends the paint no-alloc contract

`View::paint_all` opens a `pulp::runtime::ScopedNoAlloc` — "treat paint like the
audio thread". A test binary that links
`test/native_components/rt_intercept_test_support.cpp` overrides the **global**
`operator new` and **aborts** on any allocation inside that scope. That override
is per-binary, not per-test, so linking it to police one `Processor::process()`
case also arms it for every paint the binary performs.

The paint path has never satisfied that contract on any platform:

- Skia's CPU device builds an `SkPath` for each rounded rect
  (`SkBitmapDevice::drawRRect` → `SkPath::RRect` → `SkPathData::MakeNoCheck`)
- `TextShaper::resolve_typeface` builds the font-family fallback
  `vector<std::string>`

So every capture backend suspends the contract across exactly its view-tree
paint + overlay pass with `ScopedAllocAllowed` (same mechanism and rationale as
the FU-3 subtree-cache record: a non-real-time event by definition). Live
painting still runs under the contract, so a genuine per-frame allocation in
widget code is still caught.

**If you add a new capture entry point, suspend the contract in it too.** There
is more than one implementation. Apple CPU capture is in
`core/view/platform/mac/screenshot_mac.mm`; portable Linux/Windows Skia capture
is in `core/view/src/screenshot_skia.cpp`; offscreen GPU capture is in
`core/view/src/screenshot_gpu.cpp`. Patching only one file builds clean and
leaves the sibling paths exposed, so verify all backends. The regression target
`pulp-test-offscreen-capture-rt-contract` links the real Unix allocation trap
and exercises live paint plus raster, CoreGraphics, raw-RGBA, and GPU capture
where each backend is available.

**Debugging the abort:** lldb cannot catch it — the trap fires in a forked
death-test child and macOS lldb has no follow-fork-mode, so `b trap_now` + `run`
just hangs. Add `backtrace_symbols_fd` to `trap_now` in
`rt_intercept_test_support.cpp` and read the stack off stderr instead.

## DPR experiment captures are evidence, not policy

For A4 DPR v2 trials, start from `test/fixtures/gpu-ux/dpr/manifest.json` only
after its protected A3 policy status is `authorized`, and use the exact
requested scale for each capture. Keep logical size, fixture bytes,
and logical input coordinates identical while the physical backing size changes
with DPR. Record every applied DPR and exact rounded physical dimension in the
raw frame-sequence artifact bound by the v2 result contract.

`passes_content_floor` remains a required blank-frame guard, but it does not
prove small-text legibility, thin-stroke fidelity, or input correctness. Record
those oracles separately. A planned, v1, or synthetic capture is automation
proof only and must not select a scale-policy candidate.

Use `tools/scripts/gpu_dpr_runner.py` to execute or ingest a matrix cell. A
screenshot adapter must return the raw capture and content/fidelity oracles in
the cell directory; SKIP, INCONCLUSIVE, timeout, or a rejected receipt remains
resumable incomplete evidence.

In terminal v2, a capture path string is never evidence. The runner snapshots
both PNGs as unique owned regular files, verifies every PNG chunk and physical
dimension, hashes exact bytes, and recomputes similarity/text/stroke fidelity
from the retained pair. The capture/reference paths may not escape the run
root, traverse a symlink, share an inode with another artifact/cell, or change
between receipt and snapshot. `finalize-v2` accepts no caller draft and replays
all 84 plus 84 snapshot pairs; see `docs/validation/gpu-dpr/README.md`.

For the frozen Pulp-native fixtures,
`tools/scripts/gpu_dpr_pulp_native_adapter.py` accepts either a capture-only
preflight or the dedicated `pulp-gpu-dpr-native-measurement` producer. Build
that target with `PULP_BENCHMARK=ON` and `PULP_TRACING=ON`, then set
`PULP_DPR_NATIVE_MEASUREMENT_BIN` to its absolute path. Terminal first-frame
samples require 20 fresh processes whose typed rows bind attempt nonce/number,
unique PID, producer/content/build digests, exact Dawn adapter, and sample. The
same WidgetBridge/editor-surface session owns capture, logical input, 30 steady
metrics, and the correlated trace. With only `PULP_DPR_SCREENSHOT_BIN`, the
adapter retains a real preflight but deliberately returns INCONCLUSIVE; never
rename subprocess wall time to CPU/frame/interaction time.

Terminal DPR evidence additionally requires all 84 original plus all 84 repeat
cells and the named trace answers to report
one `category_scope` whose evidence ID and stable Perfetto process instance are
shared across startup, health, and probe. Global or unrelated-process trace
categories cannot complete a cell.

Treat the two fidelity images as independent evidence artifacts: hash both,
bind them to the same content/state token, and compute numeric pixel similarity,
small-text luminance variation, and thin-stroke coverage inside the frozen
scenario-specific logical ROIs. Whole-frame variance/coverage and a producer-authored
boolean is not an oracle. Logical input is equally independent: take the
expected logical point/target from the frozen scenario, then compare it with the
physical pointer event and hit target reported by the runtime. Every metric must
name `measured`, `derived`, or `unavailable`; unavailable values have no samples
or fake percentiles and cannot select policy. Preserve superseded captures as
`NONCOUNTED` in `docs/validation/gpu-dpr/instrument-validity-state.json`.

The runner caps each adapter stdout and stderr stream at 1 MiB and terminates
the adapter process group when either cap is crossed. The Pulp-native adapter
applies the same per-stream cap to its product measurement producer. An output
cap or timeout is resumable incomplete evidence; inspect the nonce-specific
logs instead of increasing the cap to accommodate sample data.
