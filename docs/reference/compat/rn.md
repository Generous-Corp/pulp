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
