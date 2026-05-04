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
