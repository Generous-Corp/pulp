---
name: screenshot
description: Capture faithful PNGs of Pulp view trees / imported UIs headlessly. Covers render_to_png backends (Skia vs CoreGraphics), the image-compositing gotcha, capture_png from a live GPU host, and the --screenshot-backend validate flag. Use whenever you render a UI to a PNG to eyeball or montage it.
---

# Screenshotting Pulp UIs

Pulp has two headless capture surfaces plus the live-host capture. Picking the
wrong one wastes time on renders that look broken but aren't.

## The one rule: use the **Skia** backend for anything with images

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
(reference | render) for structure + assets. Build montages with PIL.

## Capture options at a glance

- **`render_to_png` / `render_to_file`** (`screenshot.hpp`) — headless raster of
  a `View` tree, no window. macOS/iOS have native backends; Linux/Windows need a
  host-registered provider via `set_screenshot_provider()` (else empty/"unsupported",
  not a silent blank — #299). Probe `has_screenshot_provider()`.
- **`WindowHost::capture_png()`** — reads the rendered frame from a live GPU host
  (`MacGpuWindowHost`). The most faithful (real paint path, GPU). Drive it from
  the design-tool example's `--no-show-window --automation-before <png>`
  offscreen path, or any host that exposes capture. NOTE: the design-tool's
  `--script` expects its own entry-module shape — a raw generated `ui.js` from
  `import-design` does not load that way (throws). Prefer `--validate` with the
  Skia backend for an imported `ui.js`.
- **`pulp::view::render_to_file`** in tests — headless view-tree PNGs in CI.

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

## Gotchas

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
