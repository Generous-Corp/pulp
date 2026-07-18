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
| `skia` | **Yes** | Default for anything real — designs with assets (Figma/Pencil imports), icons, photos |
| `coregraphics` (macOS default of `default_backend`) | **No** | Vector-only UIs, or when you specifically want the CG path |
| `default_backend` | macOS → CoreGraphics, else provider | Avoid for asset-heavy UIs |

**Why it matters (the trap):** `ImageView::paint`
(`core/view/src/widgets/visualizers.cpp`) decodes images on-paint via the
canvas's `draw_image_from_file` / `measure_image_from_file` primitive.
`SkiaCanvas` implements it; the **CoreGraphics canvas does not**. On the CG
path every `ImageView` falls back to drawing its **filename as placeholder
text** — so an asset-rich import renders as empty boxes with `*.png` strings
scattered across it. That looks like "missing images / broken importer," but
the import is fine — it's the backend. Vector widgets (knobs, faders drawn via
canvas primitives) render on both backends, which makes the CG render
*partially* right and even more misleading.

Confirmed 2026-06-02 on the ELYSIUM Figma import: CoreGraphics → empty vessel
boxes + filename text; Skia → the gradient beakers/knobs/curves all composite
and the montage matches the Figma reference.

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

## Headless GPU capture: use the offscreen `gpu` backend, NOT a live window

The live-host path (`WindowHost::capture_png()`, and the standalone
`--screenshot=PATH` flag it backs) is driven per-vsync by the macOS GPU host's
`CVDisplayLink`. That clock only ticks for an **on-screen window in an
interactive WindowServer session**. In a headless / SSH / CI / agent context —
or for an `initially_hidden` accessory app — no vsync is vended, so the idle
pump never fires, the one-shot capture never runs, and `run_event_loop()`
blocks forever (the process sits in `[NSApplication run] →
nextEventMatchingMask`). Showing the window does NOT help when there's no
interactive session.

So for a **headless** capture of a GPU-rendered view, use the offscreen surface
— `render_to_file(root, w, h, path, scale, ScreenshotBackend::gpu)` /
`render_to_png_gpu` (Dawn+Skia `HeadlessSurface`, no window, no display link).
It renders the same tree through the real GPU stack and tears down cleanly, so
it neither hangs nor wedges the GPU. Reserve the live `--screenshot` path for a
real desktop session where you actually want to prove the on-screen window.

`examples/PulpTempoSampler` is the worked example of all three:
`pulp-tempo-sampler-shot OUT.png` (CPU raster, default), `… OUT.png --gpu`
(offscreen GPU, headless), and the standalone
`PulpTempoSampler … --screenshot=OUT.png` (live host window, interactive
session only).

## Gotchas

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
- Never show the user a CoreGraphics render of an asset-rich design and call it
  the import result — re-render with Skia first.
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
