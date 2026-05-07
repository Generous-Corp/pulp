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

## Counts (2026-05-07)

| Status | Count |
|--------|------:|
| supported | 78 |
| partial | 29 |
| missing | 5 |
| wontfix | 7 |
| noop | 1 |

## Recently changed

- **2026-05-07 (catalog-drift mapsTo cleanup, refs pulp #1434)** —
  paperwork-only sweep aligning catalog `mapsTo` text and the harness
  oracle's `bridgeFunctions.registered` list with what `widget_bridge.cpp`
  actually registers. Seven bridge fns that landed across #1506 / #1549 /
  #1519 / A4 #1611 (`setDirection`, `setMixBlendMode`, `setOutlineColor`,
  `setOutlineOffset`, `setOutlineStyle`, `setOutlineWidth`, `setFontFamily`,
  `setFontVariant`) were missing from the oracle's `registered` list, so
  six rn entries (`direction`, `mixBlendMode`, `outlineColor`,
  `outlineOffset`, `outlineStyle`, `outlineWidth`) tripped the
  "mapsTo cites bridge fn(s) not in widget_bridge.cpp" DIVERGE check
  even though the bridge was honest. `rn/direction`'s mapsTo also cited
  the Skia internal `paragraph_style.setTextDirection` parenthetically;
  rephrased to avoid the false-positive `setX` regex match. `rn/fontVariant`
  flipped its mapsTo + notes to reflect that the bridge fn IS registered
  (storage-only) — paint-time HarfBuzz `hb_feature_t` honoring stays
  on the unsupportedValues list so the entry honestly stays `partial`
  (storage exists, paint pipeline doesn't). No C++ / TS source change;
  `compat.json` + `tools/harness/oracles/rn/rn-viewstyle.json` only.
  Harness drift on rn drops 23 → 18.

- **2026-05-07 (pulp #1434 rn NOT-IMPL bundle 1)** — closes the eight
  remaining rn-surface NOT-IMPL entries on the harness:
  `boxSizing` (already wired since #1545; catalog flipped supported),
  `direction` (value-disambiguated — writing-direction keywords
  `ltr`/`rtl`/`inherit`/`auto` route to `setDirection` per #1506,
  flexDirection aliases `row`/`column`/`row-reverse`/`column-reverse`
  keep routing to `setFlex(direction)` for backward compat with
  prop-applier-direction.test.ts:60),
  `experimental_backgroundImage` (RN gradient string aliased to
  `setBackgroundGradient`; partial — array-of-objects gradient form
  and `url()` raster images deferred),
  `fontVariant` (OpenType feature array joined to CSV and forwarded
  to a planned `setFontVariant` bridge fn; partial — paint-time
  HarfBuzz `hb_feature_t` shape-pass wiring deferred),
  `textDecorationLine` (aliased to existing `setTextDecoration`;
  compound `'underline line-through'` forwards verbatim, single-keyword
  honored — same partial gap as `css/textDecoration`),
  `textShadowColor` / `textShadowOffset` / `textShadowRadius` (per-attribute
  setters dispatch from JSX so prop diffs preserve sibling slots; partial
  — bridge fn registration staged on the #1548 feature branch, paint-time
  glyph shadow lands when that merges). Test file
  `prop-applier-rn-not-impl-bundle1.test.ts` (21 cases) pins each
  dispatch shape including the `direction` flexDirection-alias
  backward-compat invariant. Harness drift on the rn surface drops by 7
  NOT-IMPL → 0 NOT-IMPL; PASS + DIVERGE + NO-OP rises by 7.

- **2026-05-06 (pulp #1434 Phase A2-3)** — `rn/direction` partial via
  the `@pulp/react` `writingDirection` prop. JSX surface uses RN's
  `writingDirection` (CSS `direction` already routes through
  FlexProps in this codebase); both the CSS-string-form
  `style.direction = 'rtl'` and the JSX `writingDirection` reach the
  shared `setDirection` bridge fn. Yoga honors via
  `YGNodeStyleSetDirection`. RN logical-edge bundle (#1497) keeps its
  LTR-only fast-path; RTL-aware mapping in the prop-applier is a
  follow-up. `rn/writingDirection` flagged `wontfix` per the harness
  oracle (iOS-only RN spec; cross-platform OOS).
- **2026-05-06 (pulp #1549)** — `rn/mixBlendMode` flipped `missing` →
  `supported`. RN's New Architecture `mixBlendMode` style prop now
  reaches the bridge via the `@pulp/react` prop-applier (`prop-applier.ts`
  since #1549) → `setMixBlendMode(id, kw)` (`widget_bridge.cpp`).
  The bridge maps the 16 W3C separable + non-separable blend modes
  (`normal`, `multiply`, `screen`, `overlay`, `darken`, `lighten`,
  `color-dodge`, `color-burn`, `hard-light`, `soft-light`, `difference`,
  `exclusion`, `hue`, `saturation`, `color`, `luminosity`) onto the
  shared `canvas::Canvas::BlendMode` enum and writes to a new
  `View::mix_blend_mode_` slot. `View::paint_all` forces a saveLayer
  via the new `Canvas::save_layer_with_blend()` API when the slot is
  non-default, so the subtree composites back through the requested
  mode at restore() time. Skia honors the keyword on the layer-paint;
  CG / D2D currently fall through to plain `save_layer()` (no-op
  blend) until those backends grow the same shim. `plus-lighter` /
  `plus-darker` are documented as `unsupportedValues`.
- **2026-05-06 (pulp #1546)** — five RN entries reclassified in
  compat.json: four shadow props flipped `missing` → `wontfix`,
  `isolation` flipped `missing` → `noop` (corrected from the
  initial `wontfix` after a Codex P2 review on PR #1565 noted the
  RN oracle DOES model isolation — `rn-viewstyle.json:134` defines
  enum `auto`|`isolate`, flagged "RN New Architecture only"). PURE
  CATALOG: zero implementation, zero behavior change. The four
  iOS-only legacy shadow props (`shadowColor`, `shadowOffset`,
  `shadowOpacity`, `shadowRadius`) are superseded by `boxShadow`
  (CSS box-shadow syntax) in modern RN (RN 0.71+); pulp already
  supports `boxShadow` (`rn/boxShadow=supported`), so the iOS
  legacy is genuinely out of scope rather than missing. `isolation`
  is honestly modeled as `noop` (Pulp accepts the value but the
  underlying box composition isn't stacking-context-isolated —
  same pattern as `css/animation`, `css/willChange`), keeping it
  in the rn denominator so a real implementation gap stays visible
  in drift metrics. The harness adapter short-circuits
  `status: wontfix` → OOS at step 0 (no adapter change needed);
  `noop` flows through the existing NO_OP marker path via the
  mapsTo string. Effective rn denominator (non-`wontfix`) drops
  117 → 113; harness drift count drops 21 → 17 (the four
  iOS-platformOnly entries were classifying OOS via the oracle's
  `platformOnly: ios` flag while the catalog claimed `missing`,
  producing four DRIFT entries that the explicit `wontfix` now
  resolves cleanly; isolation classifies cleanly as NO_OP).
- **2026-05-06 (pulp #1550)** — RN catalog hygiene partial → supported
  promotion pass. `rn/boxShadow`, `rn/cursor`, and `rn/overflow` flipped
  `partial` → `supported` after auditing the prop-applier dispatch path
  against the C++ bridge. All three are end-to-end wired: the prop-applier
  forwards values verbatim, the bridge fns mutate the matching View
  slots, and Skia / hit-testing honor them. Each got a `tests` reference
  added (`prop-applier-box-shadow.test.ts`,
  `prop-applier-rn-wires.test.ts`, `prop-applier-pointer.test.ts`) —
  these tests already exist; the catalog just wasn't claiming them.
  `unsupportedValues` cleaned up where over-broad: boxShadow keeps the
  honest multi-shadow / array-form gap; overflow keeps `scroll`
  (View::Overflow has no scroll mode — that's a ScrollView-intrinsic
  concern). `rn/userSelect` was audited in the same pass but
  intentionally stays `partial`: `setUserSelect` is registered as a
  no-op stash in `widget_bridge.cpp` (the prop-applier dispatches but
  the View has no UserSelect storage yet), so promotion is blocked
  until the bridge actually applies the mode. `rn/tintColor` and
  `rn/objectFit` (called out in #1550's issue body) are not in the
  catalog and not wired in prop-applier — Image bridge has the
  underlying Skia capability but no JSX → bridge dispatch — so they're
  out of scope here and stay tracked as a future Image-prop wire-up.
  Net catalog effect: 3 entries promoted partial → supported. PASS+DIV
  ratio is unchanged (DIVERGE entries still count toward effective
  coverage), but the surface report is more honest.
- **2026-05-06 (pulp #1434 Phase A2-4)** — `rn/filter` extended from
  `blur(Npx)`-only to the full CSS Filter Effects function set
  (`brightness` / `contrast` / `grayscale` / `hue-rotate` / `invert` /
  `opacity` / `saturate` / `sepia` / `drop-shadow` plus chains). The
  `@pulp/react` prop-applier already forwards the CSS string verbatim
  (sub-agent #27's bridge-wires bundle); the bridge now parses the
  full chain and routes to Skia's `SkImageFilters::Compose` +
  `SkColorFilters::Matrix` via the new
  `canvas.save_layer_with_filters(...)` API.
- **2026-05-06 (pulp #1547)** — RN textDecoration cluster surfaced at
  the `@pulp/react` JSX layer: `textDecorationLine`,
  `textDecorationColor`, `textDecorationStyle` all flipped `missing` →
  `supported`. Each prop routes through the matching bridge fn
  (`setTextDecoration` / `setTextDecorationColor` /
  `setTextDecorationStyle`) which has been registered since #1434 —
  the gap was purely on the prop-applier dispatch. RN's multi-line
  spelling `'underline line-through'` for `textDecorationLine` passes
  through verbatim; the C++ side parses it. Same change wired
  `textAlignVertical` (Android-only in RN, no CSS analogue) →
  `partial`: the four enum values map to `alignItems` on the owning
  View (top → flex-start, center → center, bottom → flex-end,
  auto → auto). RN's spec applies WITHIN a text container, but pulp's
  flex-only View model has no inline text-block concept, so this is
  the closest semantic — a View with one text child gets the same
  visual top/center/bottom alignment.
- **2026-05-06 (pulp #1518)** — `rn/flex` shorthand wired through the
  `@pulp/react` prop-applier. RN-style numeric `flex={n}` now expands
  to `{flexGrow: n, flexShrink: 1, flexBasis: 0}` (positive `n`),
  `(0, 0, 'auto')` (zero), or `(0, 1, 'auto')` (negative). Previously
  `<View flex={1} />` silently dropped the prop and consumers had to
  fan it out manually. Pure adapter aliasing — no bridge or C++ work
  needed; underlying `flex_grow` / `flex_shrink` / `flex_basis` were
  already stable. Closes the most-cited rn gap (the doc had this
  under "notable gaps" since the rn surface was first inventoried).
- **2026-05-06 (pulp #1519)** — RN outline cluster surfaced at the
  `@pulp/react` JSX layer: `outlineColor`, `outlineOffset`,
  `outlineStyle`, `outlineWidth` all flipped `missing` → `supported`.
  Each prop routes through its own per-attribute bridge fn
  (`setOutlineColor` / `setOutlineOffset` / `setOutlineStyle` /
  `setOutlineWidth`) so a JSX prop diff that touches one outline-*
  preserves the others — same shape as the borderColor / borderWidth
  / borderStyle cluster (#1027 + #1434 Triage #10). The View grew
  four new slots (`outline_color_`, `outline_offset_`,
  `outline_style_`, `outline_width_`); the line-style enum is
  reused from `View::BorderStyle` since the CSS spec lists the
  identical keyword set for outline + border. Skia paint inflates
  the box by `outline_offset + outline_width / 2` and strokes —
  outline does NOT take Yoga layout space (it draws OUTSIDE the
  border-box and the parent never reserves room for it). Dashed /
  dotted install `SkDashPathEffect` at outline-stroke time; other
  named styles (double / groove / ridge / inset / outset) currently
  degrade to solid (paint-side gap, same as borderStyle); none /
  hidden / zero-width short-circuit the stroke entirely. The same
  bridge fns flipped the `css/outline*` entries (`outline`,
  `outlineColor`, `outlineOffset`, `outlineStyle`, `outlineWidth`)
  to `supported` — the CSS translator at
  `core/view/js/web-compat-style-decl.js` now fans the `outline:
  <width> <style> <color>` shorthand out to the per-attribute
  setters and routes the four longhands through them.
- **2026-05-05 (pulp #1434 small-wins bundle, Triage #7+#12+#13+#14)** —
  the `@pulp/react` prop-applier now forwards `cursor`, `userSelect`,
  and `pointerEvents` to the matching bridge fns (previously these
  three RN-style props had no prop-applier dispatch despite the
  bridge fns existing). Status on `rn/cursor`, `rn/userSelect`, and
  `rn/pointerEvents` flipped `missing` → `partial` / `supported`,
  matching the css surface coverage. `rn/flexWrap` now accepts the
  CSS keyword string set (`'wrap'` / `'wrap-reverse'` / `'nowrap'` /
  `'no-wrap'`) alongside the legacy boolean — the bridge routes
  `wrap-reverse` through Yoga's `YGWrapWrapReverse` (Triage #14).
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

1. `rn/marginStart` / `rn/marginEnd`, `rn/paddingStart` / `rn/paddingEnd`
   — direction-aware logical props not wired (Yoga RTL is also
   unsupported).
2. `rn/shadowColor` / `rn/shadowOffset` / `rn/shadowOpacity` /
   `rn/shadowRadius` — iOS shadow surface; routed via
   `boxShadow` instead.
3. `rn/elevation` — Android-only; not modeled.
4. `rn/borderCurve` — iOS 13+ continuous-corners; not modeled.
5. `rn/isolation` — not yet wired (Skia / CG can support; bridge
   plumbing absent). `rn/mixBlendMode` was wired in pulp #1549 (see
   "Recently changed" above).

## SvgPath (#994 / #1291)

The `<SvgPath>` intrinsic surfaces a typed JSX face for
`createSvgPath` + `setSvgPath` + `setSvgViewBox` + `setSvgFill` +
`setSvgStroke` + `setSvgStrokeWidth`. Use this for rendered vector
graphics — inline `<svg>` in DOM-lite (`html/svg`) is a layout-leaf
placeholder only.
