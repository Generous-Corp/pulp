# CSS compat

Status of the CSS property surface exposed by Pulp's DOM-lite consumer
(`core/view/js/web-compat-style-decl.js`). This is the runtime that
implements `el.style.X = "..."` for plugins that author UI as
HTML+CSS rather than React.

The authoritative inventory is `compat.json` (`css/*` prefix). This
page is the human-readable narrative; for the full property list with
mapping notes, supported/unsupported values, and issue links, consult
`compat.json` directly.

## Generation

Last refresh: **2026-05-04** against `origin/main` at SHA `a5f4f5ac`.

Spec walk: top ~150 properties from
[MDN CSS Reference](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/Properties)
across layout, box-model, typography, color, transforms, transitions,
animations, gradients, flex, grid, and basic SVG. Print and paged-media
specifics are out of scope.

## Counts (2026-05-04)

| Status | Count |
|--------|------:|
| supported | ~75 |
| partial | ~30 |
| missing | ~60 |
| wontfix | ~30 |

(See `_audit.counts.css` in `compat.json` for the exact totals.)

## Recently changed

- **2026-05-05 (pulp #1434 small-wins bundle, Triage #7+#12+#13+#14)** —
  four catalog/translator items combined into one PR.
  * **Triage #7 cursor enum fan-out** — `setCursor` case ladder now
    maps the full CSS keyword set to `View::CursorStyle`. Wired:
    `pointer`, `crosshair`, `text` / `vertical-text`, `grab`,
    `grabbing`, `not-allowed` / `no-drop`, `none` / `hidden`
    (invisible), `col-resize` / `ew-resize` / `e-resize` /
    `w-resize` (horizontal), `row-resize` / `ns-resize` / `n-resize`
    / `s-resize` (vertical), `nwse-resize` / `nw-resize` /
    `se-resize` (top-left diagonal), `nesw-resize` / `ne-resize` /
    `sw-resize` (top-right diagonal), `move` / `all-scroll`
    (multi-directional). Catalog flipped to `partial` — the eight
    CSS values without a `CursorStyle` slot today (`alias`, `copy`,
    `cell`, `zoom-in/out`, `help`, `wait`, `progress`,
    `context-menu`) fall back to `default` and are tracked for a
    follow-up that adds dedicated slots + platform glyphs.
  * **Triage #12 userSelect catalog trim** — `supportedValues`
    trimmed to `none` / `text` / `all` (the actual bridge surface);
    CSS-spec `auto` and `contain` were over-claimed and would
    silently drop. Status flipped `supported` → `partial`.
  * **Triage #13 pointerEvents catalog trim** — `supportedValues`
    trimmed to `auto` / `none` / `box-only` / `box-none` (the
    `View::PointerEvents` enum). The CSS SVG-spec values
    (`visible-painted` / `visible-fill` / `visible-stroke` /
    `painted` / `fill` / `stroke`) are over-claims (pulp doesn't
    render SVG via the renderer surface; SVG is layout-leaf via
    `SvgPath` widgets). Status flipped to `partial`.
  * **Triage #14 flex-wrap reverse modes** — `FlexStyle::flex_wrap`
    converted from `bool` to a tri-state `FlexWrap` enum
    (`no_wrap` / `wrap` / `wrap_reverse`) so Yoga's
    `YGWrapWrapReverse` becomes reachable. Bridge accepts the
    keyword strings (`'wrap'` / `'wrap-reverse'` / `'nowrap'` /
    `'no-wrap'`) and the legacy 0/1 numeric path. The CSS
    `flex-flow` shorthand parser now also recognizes
    `wrap-reverse` and `row-reverse` / `column-reverse`. Status
    flipped to `supported`.
- **2026-05-05 (pulp #1434 cross-surface mega-batch)** — per-edge
  `margin{Top,Right,Bottom,Left}` and `padding{Top,Right,Bottom,Left}`
  accept percent values (`'5%'`); margin also accepts `'auto'`
  (Yoga centering — `marginLeft: auto; marginRight: auto`). The CSS
  translator forwards `'NN%'` / `'auto'` strings verbatim; the bridge
  populates `FlexStyle::dim_margin_*` / `dim_padding_*` and routes
  through Yoga's native `YGNodeStyleSetMargin{Percent,Auto}` /
  `YGNodeStyleSetPaddingPercent` APIs. Yoga's padding has no `auto`
  API (margin only). The RN-style shorthand aliases
  (`marginHorizontal`/`marginVertical` /
  `paddingHorizontal`/`paddingVertical`) fan out the same coverage
  to the per-edge dispatchers. `em` / `rem` / `vh` / `vw` / `calc()`
  remain unsupported on lengths (parseCSSLength is px-only).
  Reclassified DIVERGE → PASS for 8 per-edge + 4 alias entries.
  Mirrors PR #1426 (width/height %) and PR #1451
  (top/right/bottom/left %). Net: css drift_count -12, pass +12.
- **2026-05-05 (pulp #1434 Triage #8)** — `parseCSSColor`
  (`core/view/js/css-parser.js`) now recognizes the CSS Color Module
  Level 4 modern color spaces: `oklch(L C H [/ A])`,
  `oklab(L a b [/ A])`, `lch(L C H [/ A])`, `lab(L a b [/ A])`, and
  `color(<space> r g b [/ A])` where `<space>` is one of `srgb`,
  `srgb-linear`, `display-p3`. Spike-quality: components are converted
  at parse time to gamma-encoded sRGB hex (`#rrggbb[aa]`) so the
  existing Skia hex pipeline works unchanged. OKLab uses Björn
  Ottosson's published matrices; CIE Lab uses D50→D65 Bradford
  adaptation; display-P3 uses the standard P3→XYZ→linear sRGB matrix
  product. Out-of-gamut values are clamped at the hex boundary.
  Wide-gamut HDR via `SkColor4f` is a deferred follow-up;
  `color-mix()` and `currentColor` are also deferred. Reclassified
  DIVERGE → PASS for `css/backgroundColor`, `css/color`,
  `css/borderColor` (plus seven matching `rn/*Color` entries). Figma
  copy-CSS has been emitting `oklch(...)` since 2024; v0.dev, Tailwind,
  and Claude Design emit `lab()`/`lch()` constantly.
- **2026-05-05 (pulp #1434 css catalog hygiene)** — eight catalog-only
  refreshes: `css/width` and `css/height` now list `%` in
  `supportedValues` (mirroring `yoga/width` / `yoga/height` post-#1426 —
  the CSS translator forwards `'NN%'` strings verbatim and the bridge
  routes them via `FlexStyle.dim_*` / `YGNodeStyleSet*Percent`).
  `css/backgroundSize`, `css/backgroundPosition`, `css/backgroundRepeat`,
  `css/lineClamp`, `css/webkitLineClamp`, and `css/wordWrap` flipped
  `missing` → `partial` to reflect that the JS translator (in
  `web-compat-style-decl.js`) already routes them; the bridge functions
  remain unregistered so the values silently drop at runtime, but the
  catalog status now matches the harness verdict (DIVERGE). Drift on
  these six entries is cleared. Nine remaining `css/animation*` and
  `css/touchAction` entries closed via #1475's `noop` vocabulary
  extension (see next entry).
- **2026-05-05 (pulp #1475)** — Catalog vocabulary extension. The harness
  verifier (`tools/harness/status.py`) now recognizes a fifth catalog
  status value, `noop`, which maps to the harness `NO_OP` outcome.
  Distinct from `missing` (no implementation at all) and `partial`
  (something is implemented but lacks coverage), `noop` says the bridge
  has an explicit registration but the body is intentionally a stub
  pending a future subsystem. Nine css entries flipped to the new
  status: `css/animation`, `css/animationDelay`, `css/animationDirection`,
  `css/animationDuration`, `css/animationFillMode`,
  `css/animationIterationCount`, `css/animationName`,
  `css/animationTimingFunction` (all pending the Phase A2 animations
  subsystem) and `css/touchAction` (pending gesture routing in the
  C++ hit-test path). css drift dropped by 9 entries (60 → 51).
  `css/animationPlayState` stays `missing` because its `mapsTo` is
  `"no branch"` — the bridge has no entry point for it at all, which
  is NOT_IMPL semantics, not NO-OP.
- **2026-05-05 (pulp #1434 Triage #11)** — `css/textAlign` now accepts
  `start`, `end`, `auto`, and `justify` alongside the existing `left`,
  `center`, `right`. `start`/`end` map symmetrically to `left`/`right`
  (LTR-only today). `auto` resolves at paint time to `left` (degrades
  gracefully until pulp's RTL slice lands). `justify` reaches canvas
  `TextAlign::justify`; full SkParagraph kJustify rendering is a
  follow-up (backends approximate as left until then). `match-parent`
  remains unsupported. Reclassified DIVERGE → PASS.
- **2026-05-05 (pulp #1434 Triage #15)** — `css/boxShadow` status
  flipped `supported` → `partial`. The single-shadow CSS-spec format
  (`[inset] <dx>px <dy>px <blur>px [<spread>px] <color>`) has been
  wired since issue-925 (`web-compat-style-decl.js:565`). Multi-shadow
  comma-separated lists are deferred — single-shadow path covers the
  bulk of Figma / Tailwind / v0 emissions. Drift cleared.
- **2026-05-05 (pulp #1434 batch 6)** — `css/top`, `css/right`,
  `css/bottom`, `css/left` now accept percent values (`'50%'`). The
  CSS translator passes `'NN%'` strings verbatim to the bridge; the
  bridge detects the `'%'` suffix and routes through Yoga's native
  `YGNodeStyleSetPositionPercent` via `View::top_unit_` etc. `em`,
  `rem`, `vh`, `vw` remain unsupported (entries stay DIVERGE on those
  units). Mirrors PR #1426 (`width`/`height` percent) for the View
  positional fields.
- **2026-05-05 (pulp #1434 batch 4)** — added `css/marginHorizontal`,
  `css/marginVertical`, `css/paddingHorizontal`, `css/paddingVertical`
  RN-shorthand entries. The DOM-lite el.style adapter now recognizes
  these aliases and fans them out to the matching pair of per-edge
  `setFlex` calls (`margin_left + margin_right`, etc.). Web CSS does
  not define these properties; they ship for cross-tool import-readiness
  so RN snippets pasted into a DOM-lite plugin work as written. css
  total entry count 195 → 199; progress 55.38% → 56.28%.
- `css/border` / `css/borderRadius` / `css/borderColor`: flipped from
  `partial` (clobbered each other) to `supported`. PR #1169 routed each
  through its dedicated per-attribute setter so unset siblings are
  preserved (CSS semantics).
- `css/fontFamily`: flipped to `supported`. PR #1174 splits comma-
  separated lists, strips outer quotes, and picks the first non-empty
  family before calling Skia's `SkFontMgr`. Generic fallbacks
  (`monospace`, `ui-monospace`) still don't resolve specially — Skia
  falls through to the platform default.
- `css/display`: behavior fix in PR #1167. `display: flex` now defaults
  flex-direction to `row` to match CSS web compat (Pulp's underlying
  widgets default to `column`/RN convention). Explicit `flexDirection`
  / `flex-direction` / `flex-flow` with a direction token still wins.
- `css/transition`: extended note — PR #1345 added `:hover` selector
  parsing in inline `<style>` elements, completing the transition story
  end-to-end for the common interactive case.
- `css/__hover_pseudo`: new entry. Documents the bounded `:hover`
  support in `web-compat-document.js`.

## Silent no-ops (parser exists, backend stub)

The JS adapter parses these and calls the bridge function; the bridge
function is **not registered**, so the value is silently dropped.
`typeof` guards prevent runtime errors.

1. `css/animation*` (8 props) — `setAnimation` is registered but the
   body is `(void)id; (void)prop; …` (widget_bridge.cpp:3242). All
   `@keyframes` UIs animate nothing.
2. `css/aspectRatio` — JS routes via `setFlex(id, "aspect_ratio", …)`
   but `setFlex`'s C++ switch has no `aspect_ratio` branch.
3. `css/boxSizing` — `setBoxSizing` referenced, never registered.
4. `css/lineClamp` / `css/webkitLineClamp` — `setLineClamp` referenced,
   never registered. Multi-line truncation impossible.
5. `css/backgroundSize` / `css/backgroundPosition` /
   `css/backgroundRepeat` — three setters referenced, none registered.
   Tied to `backgroundImage: url()` which is also missing.
6. `css/outline` / `css/outlineWidth` / `css/outlineColor` —
   `setOutline` referenced, never registered. Affects accessibility
   focus rings.
7. `css/textShadow` — `setTextShadow` referenced, never registered.
8. `css/wordBreak` / `css/overflowWrap` / `css/wordWrap` —
   `setWordBreak` referenced, never registered.
9. `css/touchAction` — parsed and stored on the JS Element instance
   but never propagated to the C++ hit-test.

## Known buggy-but-supported

1. `css/lineHeight` unitless multiplier (`lineHeight: 1.5`) — silently
   dropped (only `px` accepted by `parseCSSLength`).
2. `css/flexDirection` — `row-reverse` and `column-reverse` silently
   fall through to `col` default.
3. `css/flexWrap` — bool-only; `wrap-reverse` inexpressible.
4. `css/transform` — only `scale`, `rotate`, `translate`, `translateX`,
   `translateY` honored; `scale(x,y)`, `scaleX/Y`, `skewX/Y`,
   `rotateX/Y/Z` silently dropped.
5. `css/overflow: scroll` — silently maps to `hidden` (use ScrollView).
6. `css/position: fixed` / `css/position: sticky` — accepted but layout
   may not differ from `absolute` (verification follow-up open).
7. `css/opacity: 50%` — percentage suffix stripped by `parseFloat`,
   yields `50`, clamped to `1`. Visually fine, semantically wrong.
8. `css/alignItems: baseline` / `css/alignSelf: baseline` — `FlexAlign`
   enum has no baseline variant.
