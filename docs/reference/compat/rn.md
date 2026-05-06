# React Native compat

Status of the React Native style-prop surface exposed by Pulp's
`@pulp/react` consumer (`packages/pulp-react/src/prop-applier.ts`).
This is the renderer that turns RN-style JSX into bridge `setX` calls.

The authoritative inventory is `compat.json` (`rn/*` prefix).

## Generation

Last refresh: **2026-05-04** against `origin/main` at SHA `a5f4f5ac`.

Spec walk:
- [RN View Style Props](https://reactnative.dev/docs/view-style-props)
- [RN Text Style Props](https://reactnative.dev/docs/text-style-props)
- [RN Layout Props](https://reactnative.dev/docs/layout-props)
- [RN Transforms](https://reactnative.dev/docs/transforms)

## Counts (2026-05-04)

| Status | Count |
|--------|------:|
| supported | ~45 |
| partial | ~3 |
| missing | ~70 |
| wontfix | ~3 |

## Recently changed

- **2026-05-05 (pulp #1434 rn logical-edge bundle, sub-agent #27 finding)** —
  11 RN logical-flow props wired through the `@pulp/react` prop-applier
  with an LTR-only fast path: `marginStart` / `marginEnd` /
  `paddingStart` / `paddingEnd` route to the matching `*Left` /
  `*Right` per-edge bridge calls; `borderStartWidth` /
  `borderEndWidth` route to `setBorderLeftWidth` / `setBorderRightWidth`;
  `start` / `end` route to `setLeft` / `setRight`. The CSS `inset`
  shorthand fans out to top/right/bottom/left with the standard
  1/2/3/4-token expansion (numeric or percent strings forward
  verbatim). `insetBlock` → top + bottom; `insetInline` → left +
  right. All 11 entries flipped `missing` → `partial` — the LTR
  assumption is the honest `unsupportedValues` caveat (true RTL bidi
  defers to a future direction system).
- **2026-05-05 (pulp #1434 rn bridge-wires bundle, sub-agent #27 finding)** —
  7 RN-style props that already had C++ bridge fns registered but no
  `@pulp/react` JSX dispatch. TS-only PR (no C++ touched): wired
  `backfaceVisibility`, `cursor`, `filter`, `pointerEvents`,
  `textTransform`, `transformOrigin`, `userSelect` through the
  prop-applier to the matching `setX` setters. `transformOrigin`
  parses CSS strings (`'NN% NN%'` / `'NNpx NNpx'` / `'center'` /
  keyword pairs like `'left top'`) into two fractional 0..1
  coordinates before dispatch. Status flips: `backfaceVisibility`,
  `pointerEvents`, `textTransform`, `transformOrigin`, `filter` →
  `supported`; `cursor`, `userSelect` → `partial` (CSS spec covers
  more values than the bridge's enum). Highest-leverage single rn
  PR remaining in the umbrella; closes 7 entries with zero C++ work.
- **2026-05-05 (pulp #1434 Triage #10)** — `rn/borderStyle` surfaced
  at the `@pulp/react` JSX layer with the full keyword set: `solid`,
  `dashed`, `dotted`, `double`, `groove`, `ridge`, `inset`, `outset`,
  `none`, `hidden`. The prop-applier dispatches the keyword to the
  newly-registered `setBorderStyle` bridge fn; `View::BorderStyle`
  stores the enum and the Skia paint installs `SkDashPathEffect`
  for `dashed` / `dotted` at stroke time. Other named styles
  currently degrade to solid (paint-side gap). `none` / `hidden`
  skip the stroke entirely. Reclassified missing → supported. RN
  exports / Figma motion borders / v0.dev card outlines now port
  verbatim.
- **2026-05-05 (pulp #1434 sub-agent #12 follow-up)** — three deferred
  `rn/*` entries closed in one slice. `rn/alignContent` flipped
  NOT-IMPL → PASS: `@pulp/react`'s `FlexProps` now exposes
  `alignContent` (new `FlexAlignContent` type) and the prop-applier
  forwards verbatim to `setFlex(id, 'align_content', value)`. Bridge
  routes through `FlexStyle::align_content` /
  `align_content_space` to Yoga's `YGNodeStyleSetAlignContent`;
  accepts both bare and prefixed CSS spellings plus the three
  space-* values. `rn/width` and `rn/height` flipped DIVERGE → PASS
  for `'auto'` (Yoga "hug contents") — the TS surface already typed
  these as `number | string` from the percent batch, so the change
  is bridge-side: `FlexStyle::dim_*.unit == auto_` dispatches to
  `YGNodeStyleSetWidthAuto` / `SetHeightAuto`. RN snippets emitting
  `style={{ flexWrap: 'wrap', alignContent: 'space-between' }}` or
  `style={{ width: 'auto' }}` (Figma auto-layout exports, v0.dev,
  Claude Design) now port verbatim.
- **2026-05-05 (pulp #1434 cross-surface mega-batch)** — per-edge
  `margin{Top,Right,Bottom,Left}` and `padding{Top,Right,Bottom,Left}`
  on the `@pulp/react` JSX surface now accept `number | string`
  rather than just `number`: numeric values are still px, but
  percent strings (`'5%'`) and `'auto'` (margin only) now flow
  through. The RN-style shorthand aliases (`marginHorizontal` /
  `marginVertical` / `paddingHorizontal` / `paddingVertical`) accept
  the same broadened type. Bridge dispatch routes through
  `FlexStyle::dim_margin_*` / `dim_padding_*` to Yoga's
  `YGNodeStyleSetMargin{Percent,Auto}` /
  `YGNodeStyleSetPaddingPercent` APIs. Yoga's padding has no `auto`
  API. RN exports / Figma transition states / v0.dev hero margins
  using `'auto'` for centering or percent for fluid layouts now
  port verbatim. Catalog accuracy update — these entries were PASS
  already, but `supportedValues` now correctly enumerates the
  broadened type vocabulary.
- **2026-05-05 (pulp #1434 Triage #8)** — `rn/backgroundColor`,
  `rn/color`, and the per-edge `rn/border{Top,Right,Bottom,Left}Color`
  + `rn/borderColor` entries now accept the CSS Color Module Level 4
  modern color spaces: `oklch()`, `oklab()`, `lch()`, `lab()`, and
  `color(srgb|srgb-linear|display-p3 ...)`. The shared `parseCSSColor`
  helper (in `core/view/js/css-parser.js`, used by both the DOM-lite
  path and any RN style consumer that resolves color strings)
  converts to gamma-encoded sRGB hex at parse time. Reclassified
  DIVERGE / partial → PASS for 7 `rn/*Color` entries. RN `style={{
  color: 'oklch(0.7 0.18 240)' }}` ports verbatim from Figma copy-CSS,
  v0.dev hero color tokens, and Claude Design transition states.
- **2026-05-05 (pulp #1434 Triage #9 fan-out)** — `rn/transform` array
  walker now dispatches `skewX` / `skewY` (previously stored on the
  snapshot but silently dropped because the bridge fn wasn't
  registered). `setSkew(id, x_deg, y_deg)` is newly registered;
  `View::set_skew` had existed in C++ since the 2D slot landed.
  `[{skewX:'10deg'},{skewY:'5deg'}]` produces ONE consolidated
  `setSkew(10, 5)` call thanks to the within-array axis merging.
  Status flipped `partial` → `supported`. Deferred (still silent
  no-op): `rotateX` / `rotateY` / `perspective` / `matrix` — pulp's
  2D View has no 3D rotation storage; tracked for a follow-up.
- **2026-05-05 (pulp #1434 Triage #9)** — `rn/transform` now accepts
  the RN array-of-objects shape:
  ```js
  style={{ transform: [
    { translateX: 10 }, { translateY: 20 },
    { rotate: '45deg' }, { scale: 1.5 },
  ] }}
  ```
  The `@pulp/react` prop-applier walks the array in one pass,
  accumulates a `{tx, ty, rotateDeg, scale}` snapshot, then dispatches
  consolidated `setTranslate(id, x, y)` / `setRotation(id, deg)` /
  `setScale(id, s)` calls. Within-array merging preserves unrelated
  axes — `[{translateX:10},{translateY:20}]` produces ONE
  `setTranslate(10, 20)` rather than two clobbering calls.
  Wired ops: `translateX`, `translateY`, `rotate`, `rotateZ`, `scale`,
  `scaleX`, `scaleY` (last-write-wins on the uniform bridge).
  Angle units: `'45deg'`, `'0.785rad'`, `'0.5turn'`, `'50grad'`,
  numeric (deg). Deferred (silent no-op): `skewX` / `skewY` (bridge fn
  unregistered — `View::set_skew` exists), `rotateX` / `rotateY` /
  `perspective` / `matrix` (no 3D model in the 2D View). CSS string
  form (`'translateX(10px) rotate(45deg)'`) deferred — pass array.
  Status flipped `missing` → `partial`. Highest-impact rn import-
  readiness gap closed; Figma / v0 / Claude Design exports now port
  verbatim.
- **2026-05-05 (pulp #1434 Triage #11)** — `rn/textAlign` now accepts
  `auto` and `justify` alongside the existing `left`, `center`,
  `right`. `auto` is writing-direction-relative (LTR-only today —
  degrades to `left` until pulp's RTL slice lands). `justify` reaches
  canvas `TextAlign::justify`; SkParagraph kJustify rendering is a
  follow-up (backends approximate as left until then). The `@pulp/react`
  prop-applier widens `textAlign?: 'left' | 'center' | 'right'` to
  also cover `'auto' | 'justify'`. Reclassified DIVERGE → PASS.
- **2026-05-05 (pulp #1434 Triage #15)** — `rn/boxShadow` surfaced at
  the `@pulp/react` TS layer. The prop-applier accepts both the CSS
  shorthand string (`'2px 4px 8px rgba(0,0,0,0.3)'`, with optional
  spread + leading/trailing `inset`) and the RN object form
  (`{offsetX, offsetY, blur, spread, color, inset}`). Both shapes
  forward to the existing `setBoxShadow(id, dx, dy, blur, spread, color,
  inset)` bridge that has shipped since pulp #925. Sentinel `'none'` /
  `''` route to `clearBoxShadow`. Status flipped `missing` → `partial`:
  multi-shadow comma-separated lists remain deferred — the single-
  shadow path covers the bulk of Figma / Tailwind / v0 emissions.
- **2026-05-05 (pulp #1434 Triage #12)** — `rn/display` now accepts
  `'flex'` and `'none'`. The `@pulp/react` prop-applier dispatches
  `display: 'flex'` → `setVisible(id, true)` and `display: 'none'` →
  `setVisible(id, false)`. Mirrors the yoga side wired in #1422.
  Reclassified `missing` → `partial` (`'contents'` remains unsupported
  — Yoga has no equivalent).
- **2026-05-05 (pulp #1434 batch 6)** — `rn/top`, `rn/right`,
  `rn/bottom`, `rn/left` now accept percent strings (`'50%'`)
  alongside numeric pixel values. The `@pulp/react` prop-applier
  forwards the value verbatim to `setTop` / `setRight` / `setBottom` /
  `setLeft`; the bridge detects the `'%'` suffix and routes through
  Yoga's `YGNodeStyleSetPositionPercent`. Mirrors PR #1426
  (`width`/`height` percent) for the View positional fields. RN code
  using `style={{top:'50%'}}` for absolute-positioned overlays now
  ports verbatim. drift_count: 32 → 31.
- **2026-05-05 (pulp #1434 batch 4)** — RN shorthand aliases
  `rn/marginHorizontal`, `rn/marginVertical`, `rn/paddingHorizontal`,
  `rn/paddingVertical` are now surfaced at the `@pulp/react` JSX
  intrinsic. The prop-applier fans out each alias to the matching pair
  of per-edge `setFlex(margin_*|padding_*, value)` calls. Reclassified
  missing → supported (PASS on the harness rn surface). Lets RN
  snippets that write `style={{ marginHorizontal: 8 }}` port verbatim.
- New `rn/d`, `rn/viewBox`, `rn/fill`, `rn/stroke`, `rn/strokeWidth`
  entries. Wired by the `@pulp/react` `<SvgPath>` JSX intrinsic in
  PR #1291 (pulp #994). Bridge surface lands on `SvgPathWidget`.
- New `rn/overlay` entry. `overlay={true}` claims the global active-
  overlay slot for click routing on absolute-positioned popovers.
  PRs #1297 + #1344 (pulp #1148).
- Per-side flat border props (`borderTopColor`, `borderTopWidth`,
  `borderTopLeftRadius`, etc.) flipped to `supported` after PR #1169
  fixed the per-attribute setters.

## Notable gaps

1. `rn/flex` shorthand — RN's `style={{flex: 1}}` is the most common
   pattern in tutorials. `@pulp/react` users must type
   `flexGrow={1} flexShrink={1} flexBasis={0}`.
2. `rn/marginStart` / `rn/marginEnd`, `rn/paddingStart` / `rn/paddingEnd`
   — direction-aware logical props not wired (Yoga RTL is also
   unsupported).
3. `rn/shadowColor` / `rn/shadowOffset` / `rn/shadowOpacity` /
   `rn/shadowRadius` — iOS shadow surface; routed via
   `boxShadow` instead.
4. `rn/elevation` — Android-only; not modeled.
5. `rn/borderCurve` — iOS 13+ continuous-corners; not modeled.
6. `rn/mixBlendMode`, `rn/isolation` — not yet wired (Skia / CG can
   support; bridge plumbing absent).

## SvgPath (#994 / #1291)

The `<SvgPath>` intrinsic surfaces a typed JSX face for
`createSvgPath` + `setSvgPath` + `setSvgViewBox` + `setSvgFill` +
`setSvgStroke` + `setSvgStrokeWidth`. Use this for rendered vector
graphics — inline `<svg>` in DOM-lite (`html/svg`) is a layout-leaf
placeholder only.
