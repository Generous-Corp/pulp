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

1. `rn/transform` — `@pulp/react` has no `transform` prop at all today;
   the RN array-of-objects shape is not surfaced.
2. `rn/flex` shorthand — RN's `style={{flex: 1}}` is the most common
   pattern in tutorials. `@pulp/react` users must type
   `flexGrow={1} flexShrink={1} flexBasis={0}`.
3. `rn/marginStart` / `rn/marginEnd`, `rn/paddingStart` / `rn/paddingEnd`
   — direction-aware logical props not wired (Yoga RTL is also
   unsupported).
4. `rn/shadowColor` / `rn/shadowOffset` / `rn/shadowOpacity` /
   `rn/shadowRadius` — iOS shadow surface; routed via
   `boxShadow` instead.
5. `rn/elevation` — Android-only; not modeled.
6. `rn/borderCurve` — iOS 13+ continuous-corners; not modeled.
7. `rn/mixBlendMode`, `rn/isolation` — not yet wired (Skia / CG can
   support; bridge plumbing absent).

## SvgPath (#994 / #1291)

The `<SvgPath>` intrinsic surfaces a typed JSX face for
`createSvgPath` + `setSvgPath` + `setSvgViewBox` + `setSvgFill` +
`setSvgStroke` + `setSvgStrokeWidth`. Use this for rendered vector
graphics — inline `<svg>` in DOM-lite (`html/svg`) is a layout-leaf
placeholder only.
