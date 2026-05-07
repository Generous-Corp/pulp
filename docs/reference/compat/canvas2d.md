# Canvas2D compat

Status of the Canvas2D rendering surface exposed by
`core/view/js/web-compat-canvas.js` and the `canvas*` family in
`core/view/src/widget_bridge.cpp`. Backed by the Skia canvas
(`core/canvas/src/skia_canvas.cpp`) on Linux and Windows, and the
Core Graphics canvas (`core/canvas/platform/mac/cg_canvas.mm`) on
macOS.

The authoritative inventory is `compat.json` (`canvas2d/*` prefix).

## Generation

Last refresh: **2026-05-04** against `origin/main` at SHA `a5f4f5ac`.

Spec walk:
[MDN CanvasRenderingContext2D](https://developer.mozilla.org/en-US/docs/Web/API/CanvasRenderingContext2D)
(HTML Living Standard).

This section was previously a stub. The 2026-05-04 pass walked the
~36 prototype methods + 7 attrs in the JS shim against the ~47
`canvas*` register_function entries in widget_bridge.cpp, with extra
notes per backend where Skia and CG diverge.

## Counts (2026-05-04)

| Status | Count |
|--------|------:|
| supported | ~38 |
| partial | ~10 |
| missing | ~12 |

## Recently expanded (#1348)

PR #1348 (pulp #1346, augments #1322) wired up the bulk of the
Canvas2D state-stack and transform surface that FilterBank's
visualizer needs:

- State stack: `save`, `restore`
- Transform: `translate`, `scale`, `rotate`, `setTransform`,
  `resetTransform`, `transform` (pure-translation only)
- Path construction: `arc`, `arcTo`, `bezierCurveTo`,
  `quadraticCurveTo`, `rect`, `roundRect`, `ellipse`, `clip`
- Text: `fillText`, `strokeText`
- Gradients: `createLinearGradient`, `createRadialGradient`,
  `addColorStop`
- Compositing: `globalAlpha`, `globalCompositeOperation`
- Line state: `lineCap`, `lineJoin`, `setLineDash` / `getLineDash`

Skia gradient-shape fills (fillRect / fillCircle /
fillRoundedRect honoring an active linear or radial gradient) landed
in #1353. CG counterpart in #1359 / PR #1360 (merged 2026-05-03).

## Pre-existing surface

These were wired before #1348 and remain stable:
`fillRect`, `strokeRect`, `clearRect`, `beginPath`, `closePath`,
`moveTo`, `lineTo`, `fill`, `stroke`, `lineWidth`, `textAlign`,
`textBaseline`, `fillStyle`, `strokeStyle`, `measureText`,
`drawImage`, `getImageData`, `putImageData`. Pulp-specific bridge
conveniences: `canvasFillCircle`, `canvasFillRoundedRect`,
`canvasStrokeLine`.

## Notable gaps

The two former NOT-IMPL entries (`filter`, `direction`) flipped to
`partial` in pulp #1520 — see "Recently wired" below for scope.

## Recently expanded — full CSS `font` shorthand (pulp #1434)

The Canvas2D `ctx.font` setter previously only parsed the legacy
`'<size>px <family>'` form, so every Figma copy-CSS value of the shape
`'italic small-caps bold 14px/1.4 "Inter", sans-serif'` collapsed to
`14px sans-serif` (size + first family token only). The 2026-05-05
shim parser now walks the full CSS Fonts Module Level 4 grammar:

```
font: [<font-style>] [<font-variant>] [<font-weight>] [<font-stretch>]
      <font-size>[/<line-height>] <font-family>
```

Plumbing:

- `canvasSetFontFull(id, family, size, weight, slant, letterSpacing)` —
  new bridge fn that records a `set_font_full` cmd carrying weight and
  slant. Replays through `Canvas::set_font_full`, which Skia honours via
  `SkFontStyle(weight, kNormal_Width, italic_or_upright)`. CG falls
  through to family + size (the base default).
- `canvasSetFont(id, family, size)` — kept as the legacy fallback for
  hosts that pre-date this PR.

Supported tokens:

- Style: `normal`, `italic`, `oblique` (slant flag).
- Variant: `small-caps` parsed and tracked locally; not yet plumbed to
  Skia/CG (no canvas-API surface for variant).
- Weight: keywords `normal`/`bold`/`bolder`/`lighter`, and 3-digit
  numeric `100`..`900`.
- Stretch keywords: parsed and dropped (no canvas-API surface).
- Size: `<n>px` (mandatory). `pt`/`em`/`rem` accepted by the regex but
  the bridge expects px — non-px tokens silently keep the default 14.
- Line-height: number, length, or `normal`. Per Canvas2D spec, the
  parsed value is ignored — canvas always uses the font's own metrics.
  Tracked on `_parsedLineHeight` for round-tripping.
- Family: comma-separated lists pass through verbatim. Single-family
  strings have surrounding `"`/`'` quotes unwrapped.

The `font` getter still returns the originally-assigned string verbatim,
so the spec round-trip (`ctx.font = 'italic 14px Inter'; ctx.font`)
returns the same string.

## Recently wired — `fillRule` arg on `fill()` / `clip()` (pulp #1522)

`ctx.fill('evenodd')` and `ctx.clip('evenodd')` now produce different
rasterized output than the spec default. Previously the JS shim
discarded the arg (`void fillRule;`), the bridge had no overload for
it, and Skia / CG always painted with non-zero winding.

Plumbing:

- JS shim encodes the arg as an int (`0` = nonzero / spec default,
  `1` = evenodd) and threads it through `canvasFillPath(id, rule)` /
  `canvasClip(id, rule)`.
- Bridge stores it on `CanvasDrawCmd::int_val`.
- Skia: `SkPathBuilder::setFillType(kEvenOdd | kWinding)` immediately
  before `drawPath` / `clipPath`.
- CG: `CGContextEOFillPath` / `CGContextEOClip` for evenodd, default
  `CGContextFillPath` / `CGContextClip` otherwise.

`compat.json`: `canvas2d/fill` and `canvas2d/clip` are now `supported`
(both promoted from `partial` by this change).

## Recently wired (pulp #1434 bridge-thin gap-fill)

The 2026-05-05 bridge-thin gap-fill PR closed four NOT-IMPL entries
that were blocked only on a missing bridge fn — Skia and CG already
exposed the underlying capability:

- **`createConicGradient(startAngle, x, y)`** — Skia routes through
  `SkGradientShader::MakeSweep` (real conic sweep). CG previously
  degraded to the first-stop colour; **pulp #1524** software-rasterises
  a `CGImage` of the active clip's bounding box (per-pixel `atan2`
  sweep + colour-stop interpolation) and paints it via
  `CGContextDrawImage`, so both backends now render real multi-stop
  sweeps.
- **`miterLimit`** — `SkPaint::setStrokeMiter` (Skia) /
  `CGContextSetMiterLimit` (CG). Sticky stroke state. Spec: non-positive
  / non-finite values silently ignored.
- **`imageSmoothingEnabled`** + **`imageSmoothingQuality`** —
  Skia: `SkSamplingOptions` (`kLinear`, `+mipmap`, Mitchell cubic).
  CG: `CGContextSetInterpolationQuality` (`Low` / `Medium` / `High`).
  `enabled = false` collapses to nearest-neighbour for pixel-art.

### Follow-up: `createPattern` (sub-agent #24)

The follow-up bridge-thin slice wired `createPattern` — deferred from
the original #1480 PR because pattern handling needs an image-resource
identifier shape, not just a bridge fn:

- **`createPattern(image, repetition)`** — Skia routes through
  `SkShader::MakeImage` with `SkTileMode::kRepeat` / `SkTileMode::kDecal`
  per axis. All four spec repetition values are supported:
  `"repeat"`, `"repeat-x"`, `"repeat-y"`, `"no-repeat"`. **pulp #1524**
  promoted the CG path from solid-fill fallback to a real
  `CGPatternCreate` + `CGPatternCallbacks` (image-tile draw callback)
  + `CGContextSetFillPattern` flow; `no_repeat` axes blow up the tile
  step beyond the clip bounding box so only the seed tile lands.

### Promotion: pulp #1524 — CG-degraded gradient/pattern cluster

3 entries previously DIVERGE on the CG backend now PASS on both Skia
and CG:

- **`createRadialGradient(x0,y0,r0,x1,y1,r1)`** — full two-circle form.
  Skia routes through `SkGradientShader::MakeTwoPointConical`; CG
  routes through `CGContextDrawRadialGradient(start_centre, start_radius,
  end_centre, end_radius)` with both circles wired. The JS shim
  flushes via the new `canvasSetRadialGradientTwoCircles` bridge fn
  (single-circle path retained for legacy hot-reloaded bundles).
- **`createConicGradient(startAngle, x, y)`** — CG software-rasterises
  the sweep into a `CGImage` (see above).
- **`createPattern(image, repetition)`** — CG installs a real
  `CGPattern` (see above).

Side effect: the same PR plumbed `setStrokeCap` and `setStrokeJoin`
through `SkPaint` — `line_cap_` and `line_join_` were stored on the
Skia canvas but never applied to the actual paint. `lineCap` and
`lineJoin` are now visually faithful on the GPU path.

The Canvas2D `shadowColor` / `shadowBlur` / `shadowOffsetX` /
`shadowOffsetY` quartet landed in pulp #1434 batch 7 (separate slice).

### Follow-up: `direction` + `filter` (pulp #1520)

The two final NOT-IMPL entries in the canvas2d catalog flipped to
`partial`:

- **`direction`** — JS shim tracks `ltr` / `rtl` / `inherit` and
  flushes via `canvasSetDirection`. Skia wires the enum through to the
  `SkShaper` `leftToRight` flag so HarfBuzz emits glyphs in visual order
  for right-to-left scripts. `inherit` maps to `ltr` until per-View
  writing-direction lookup lands (#1506); full Unicode bidi for
  mixed-script paragraphs requires SkParagraph plumbing tracked there.
- **`filter`** — JS shim tracks the raw CSS `<filter-function-list>`
  string and flushes via `canvasSetFilter`. Skia parses the string into
  an `SkImageFilter` chain and applies via `SkPaint::setImageFilter` on
  every subsequent fill / stroke / text / image draw. Supported
  functions: `blur`, `brightness`, `contrast`, `grayscale`,
  `hue-rotate`, `invert`, `opacity`, `saturate`, `sepia`. The
  `drop-shadow(...)` form is deferred (needs offset+blur+color tuple
  parsing) and `url(#filter-id)` SVG references are out-of-scope.

Both setters are sticky and stack with `save()` / `restore()`. The CG
backend stores the values but renders unfiltered today (Skia-only
filter chain); a CG follow-up can route through `CGContextSetShadow`
+ `CGImage`-backed colour-matrix passes when needed.

### Recently changed (pulp #1527)

The bridge-thin gap-fill PR closed the three remaining
synchronous-return Canvas2D entries — the underlying Skia/CG paths
were already there; the gap was the JS-side mirror needed for spec
semantics.

- **`getTransform()`** — returns a DOMMatrix-shaped object reflecting
  the current 2D affine transform. The shim mirrors `_currentTransform`
  via `translate` / `scale` / `rotate` / `setTransform` / `transform`
  and the `save` / `restore` stack so the read is synchronous (the
  bridge replays draws at paint time and has no queryable "current
  matrix" from JS). The returned object exposes `a, b, c, d, e, f`,
  the `m11..m44` DOMMatrix aliases, `is2D`, `isIdentity`, and
  `toFloat32Array` / `toFloat64Array`. Mutating the returned matrix
  does NOT affect the live ctx (spec-compliant — `getTransform`
  returns a NEW DOMMatrix per call).
- **`isPointInPath(x, y[, fillRule])`** — JS-side ray-cast against the
  path mirror. Each path-construction method (`moveTo`, `lineTo`,
  `rect`, `roundRect`, `arc`, `arcTo`, `ellipse`, `bezierCurveTo`,
  `quadraticCurveTo`, `closePath`) appends to `_pathSubpaths` in
  addition to forwarding to the bridge. Curve segments sample to ~16
  line pieces — accurate enough for spec-compliant hit testing without
  pulling a real bezier solver into JS. The path mirror is part of the
  `save` / `restore` snapshot. Path2D-object first argument and
  fillRule="evenodd" semantics for self-intersecting paths are
  out-of-scope (the ray-cast returns the same answer for both rules
  on simple non-self-intersecting paths).
- **`isPointInStroke(x, y)`** — closest-point-on-segment distance check
  against the same path mirror; returns `true` when the test point's
  distance to any path segment is ≤ `lineWidth / 2`. Respects later
  assignments to `ctx.lineWidth`. Approximates as round-cap geometry;
  square caps and miter spikes are not modelled.

## Partial / approximated

1. **`arcTo`** — currently emits `lineTo(x1,y1) + lineTo(x2,y2)`,
   ignoring the radius. Used by `roundRect` and FilterBank's marquee
   corners; fidelity loss is minimal for those use cases.
2. **`ellipse`** — ignores `rotation`; falls through to `arc` when
   `rx === ry`; otherwise emits a 4-segment cubic approximation.
3. **`transform`** (concat-on-right) — bridge only exposes `setTransform`
   (replace). Pure-translation matrices fall through to `translate`;
   anything else is a no-op.
4. **`strokeText`** — falls back to `fillText` with the stroke color
   (bridge has no stroke-text command).
5. **`drawImage`** 9-arg form — sprite-sheet slicing
   (`sx, sy, sw, sh`) is currently ignored.
6. **`putImageData`** sub-rect form — `dirtyX/Y/W/H` ignored.
7. **`setLineDash`** — odd-length spec-doubling is bridge-side; shim
   passes verbatim. `lineDashOffset` mutation between draws is not
   re-pushed (must re-call `setLineDash`).
8. **`strokeStyle = gradient`** — bridge has no stroke-gradient setter;
   degrades to the first stop's color.

## WebGPU surface

`canvas.getContext('webgpu')` is wired through
`__createGPUCanvasContext` — bridges to native WebGPU when the device
is native. See planning notes for the Three.js + Dawn end-to-end story.
The full WebGPU surface lives separately in `web-compat-gpu-buffered.js`
and is out of scope for this Canvas2D compat doc.
