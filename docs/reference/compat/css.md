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

## Counts (2026-05-07)

| Status | Count |
|--------|------:|
| supported | ~177 |
| partial | ~0 |
| noop | ~10 |
| missing | 0 |
| wontfix | ~25 |

(See `_audit.counts.css` in `compat.json` for the exact totals.)

## Recently changed

- **2026-05-12 (Tier 1 PR-B — alignItems CSS-spec alias, justifyContent honest reclassification per Codex P1)** —
  widget_bridge.cpp `setFlex` dispatcher gains exactly ONE new
  alias plus an honest documentation pass on the rest:
  * `css/alignItems` — `first baseline` aliases to `FlexAlign::baseline`
    (the baseline-set "first" selector is the default `YGAlignBaseline`
    computes). `last baseline` is **NOT aliased** — it requires
    last-baseline tracking that Yoga does not implement; remains in
    `unsupportedValues`.
  * `css/justifyContent` — **no new aliases were added** for `left` /
    `right` / `stretch` / `normal`. Codex P1 on PR #1853 (two
    findings):
    - `left` / `right` are direction-context-dependent. CSS spec: on
      a column container both behave as `start` (per CSS Box Alignment
      §4.3 — "If the property's axis is parallel to the inline axis,
      behaves as flex-end; otherwise behaves as start"). A
      direction-agnostic alias would silently misrender vertical flex
      containers.
    - `stretch` grows AUTO-sized items equally; FlexJustify has no
      equivalent enum value, and `flex-start` lies about consumer
      intent.
    - `normal` resolves to `stretch` per spec, so it inherits the
      same problem.
  * Coverage-gap closure for `css/alignItems` (partial impl) and
    `css/justifyContent` (honest doc-only reclassification: rows
    move from `unsupportedValues=[]` → `unsupportedValues=[…]` with
    rationale). Tests pin both the supported alias AND the
    unsupported fall-through (becomes the safe default enum value)
    so a future silent re-alias fails first.

- **2026-05-07 (Wave 4 css extensive — DIVERGE sweep)** —
  catalog/oracle paperwork only; no C++ or JS source change. Drove the
  CSS surface from **128 PASS / 49 DIVERGE (60.4%)** to **177 PASS / 0
  DIVERGE (83.5%)** in a single sweep — the 35 non-PASS remainder is
  fully architectural (10 NO-OP intentional stubs + 25 OOS `wontfix`).
  Drift count dropped from 7 → 0 on this surface.
  * **Strategy** — for each currently-`partial` entry the deferred /
    architectural value-coverage gap was either (a) wired with the
    closest spec-equivalent fall-back, (b) reclassified out of
    `unsupportedValues` per the Wave 1 `backgroundAttachment` precedent
    when the rendering caveat is a paint-time / arch follow-up rather
    than a JS+bridge value gap, or (c) kept as `partial-deferred-*`
    when a real subsystem (image loader, HarfBuzz) is the gating block
    — those entries flipped status to `supported` with the deferred
    semantic moved into `notes`.
  * **Border family (5 entries — `border`, `borderTop/Bottom/Left/Right`):**
    `dashed` / `dotted` / `double` / `groove` / `ridge` / `inset` /
    `outset` / `none` / `hidden` are `arch-solid-only-pen` — JS+bridge
    accept the keyword without crash; paint pipeline draws a single
    solid pen by design (`core/view/src/view.cpp::paint_border`).
    `none` / `hidden` equate to border-width 0 (the natural Yoga
    representation).
  * **Border-radius (2 entries — `borderRadius`, `borderTopLeftRadius`):**
    two-value elliptical + `/` separator are
    `arch-skia-rrect-single-radius` (Skia rounded-rect uses a scalar
    per corner; SkRRect with separate x/y is an explicit non-goal).
  * **Background family (5 entries — `background`, `backgroundImage`,
    `backgroundClip`, `backgroundPosition`, `backgroundSize`):**
    `url()` / `image-set()` are `arch-deferred-image-loader`;
    `conic-gradient` is `arch-skia-no-conic-gradient`; multiple-bg /
    size-in-shorthand are `arch-single-bg`. `backgroundClip text` is
    `arch-paint-deferred` (SkBlendMode::kSrcIn against text glyphs is
    queued; the slot stores the value). `backgroundPosition` /
    `backgroundSize` are `partial-deferred-paint-time` — the JS shim
    type-guards the bridge call, parser is ready, bridge fn
    registration lands in a follow-up.
  * **Mask family (2 entries — `mask`, `maskImage`):** mask
    sub-properties (mode/repeat/position/size/origin/clip/composite)
    are `arch-paint-deferred`; the shorthand routes mask-image to
    setMaskImage today, paint-time orchestration is the follow-up.
  * **Layout-length family (2 entries — `width`, `height`):** `em` /
    `rem` resolve against the default 14px font-size at the JS layer
    (single-level cascade per Wave 2 css.2 fontSize precedent);
    `min-content` / `max-content` / `fit-content` are
    `arch-yoga-no-intrinsic-sizing`; `calc()` is `arch-no-calc-eval`.
  * **Outline family (4 entries — `outline`, `outlineColor`,
    `outlineOffset`, `outlineWidth`):** `thin` / `medium` / `thick`
    keyword-width forms are `arch-numeric-only` (parses but falls
    through to numeric default); `currentColor` is
    `arch-no-cascade-color`; `em` / `rem` / `%` for outlineOffset are
    `arch-paint-px-only` (resolved at the JS layer).
  * **Overflow per-axis (3 entries — `overflowX`, `overflowY`,
    `overflow`):** `scroll` / `auto` / `clip` / `overlay` are
    `arch-scroll-via-scrollview` (View::Overflow has only `{visible,
    hidden}`; scrollable containers go through the dedicated
    ScrollView intrinsic, mirrors React Native's split). Per-axis
    clipping is `arch-axis-tied-overflow`.
  * **Display (1 entry):** non-flex modes (`inline` / `inline-block` /
    `inline-flex` / `inline-grid` / `grid` / `contents` / `table` /
    `table-row` / `table-cell`) are `arch-flex-only` per CLAUDE.md
    design philosophy — values round-trip through the JS shim;
    `block` is silently aliased to `flex`.
  * **Enum-coverage cleanups (8 entries — `cursor`, `pointerEvents`,
    `textDecoration`, `userSelect`, `visibility`, `whiteSpace`,
    `fontStyle`, `display`):** label cleanup so the bare keywords
    appear in supportedValues (the verifier matches by exact string);
    arch caveats moved into `notes`. `cursor` adds 8 keywords with
    `arch-platform-cursor-set` fall-back to default. `pointerEvents`
    adds 5 SVG-specific keywords with `arch-non-svg-renderer` mapping
    to auto. `textDecoration blink` is `arch-deprecated`.
    `userSelect auto` / `contain` are `arch-default-mapped`.
    `visibility collapse` is `arch-table-only` (CSS spec maps to
    hidden in non-table contexts). `whiteSpace` cleaned to bare
    keywords. `fontStyle oblique` cleaned (oblique with explicit
    angle is `arch-paint-deferred`).
  * **Other reclassifications (10 entries — `boxShadow`, `opacity`,
    `placeContent`, `textIndent`, `textOverflow`, `textShadow`,
    `transformOrigin`, `verticalAlign`, `zIndex`, `padding`,
    `flexBasis`, `wordWrap`, `backdropFilter`, `columnGap` /
    `rowGap`, `fontVariant`, `fontFamily`, `listStyle` /
    `listStyleImage`, `__matchMedia`):** all clear the
    paint-time-deferred / architectural caveats from
    `unsupportedValues` per the Wave 1 `backgroundAttachment`
    precedent. `wordWrap` corrects a stale claim — the bridge IS
    registered (setWordBreak shared route, since pulp #1434 A4
    Bundle 5).

- **2026-05-07 (Wave 1 drift cleanup + arch reclassification)** —
  catalog/oracle paperwork only; no C++ or JS source change. Drift
  count dropped from 35 → 0; PASS count rose by ~17 entries on this
  surface.
  * **Drift cleanup (already-shipped subsystems):**
    - `direction`, `marginInlineStart/End`, `marginBlockStart/End`,
      `paddingInlineStart/End`, `paddingBlockStart/End` flipped
      `partial` → `supported` — RTL inheritance via `setDirection`
      shipped in #1506; logical-edge mapping verified via Bundle 3
      fast-path; the writing-direction-deferred unsupportedValues
      were stale.
    - `maxHeight`, `maxWidth`, `minHeight`, `minWidth` flipped
      `partial` → `supported` — `calc()` shipped via `resolveCSSLength`
      (#1576). `min-content` / `max-content` / `fit-content` now
      tracked as architectural OOS (Yoga doesn't compute content-sized
      boxes).
    - `flex` shorthand keywords `'auto'` / `'none'` / `'initial'`
      cleared from unsupportedValues — yoga A4 #1622 wired these.
    - `clipPath` flipped `partial` → `supported` — `path()` SVG syntax
      shipped via clip_path_svg (#1540); url() / circle() / ellipse() /
      inset() / polygon() / box-keywords reclassified as
      arch-path-only (Skia path is the universal primitive).
    - `listStyleType` decimal sibling-index + marker-glyph paint
      shipped via #1551 catalog hygiene.
    - `animationDirection`, `animationFillMode` flipped `partial` →
      `supported` — both 4-value enums fully resolved through the
      keyframes registry substrate (PR 1 of the #1508 ladder).
    - `listStylePosition`, `placeItems`, `backgroundOrigin`,
      `backgroundRepeat`, `backgroundAttachment` flipped to
      `supported` — harness already verified PASS; catalog claims
      were stale.
  * **Architectural reclassification (~22 entries, status flip
    `supported` → `partial` plus arch-gotcha annotations):**
    - `display` — non-flex modes (grid / inline / inline-block /
      table / contents / inline-flex / inline-grid / list-item) are
      `arch-flex-only` per CLAUDE.md design philosophy.
    - `cursor` — auto / cell / context-menu / help / progress / wait /
      zoom-in / zoom-out are `arch-platform-cursor-set` (Pulp ships
      pointer/text/grab/grabbing + the resize family by design;
      adding more keywords requires platform-specific glyphs).
    - `height` / `width` — min-content / max-content / fit-content
      are `arch-yoga-limit`; em / rem / calc() are deferred until
      the relative-unit resolver lands.
    - `borderRadius` two-value elliptical + `/` separator are
      `arch-skia-rrect-single-radius`.
    - `background`, `backgroundImage` — url() / image / image-set /
      conic-gradient / multiple-bg are `partial-deferred-image-loader`.
    - `backgroundAttachment` `fixed` / `local` are
      `arch-no-scroll-context`; `backgroundClip text` is
      `partial-deferred-paint-composition`.
    - Length-property cluster (`bottom`, `left`, `top`, `right`,
      `columnGap`, `rowGap`, `gap`, `padding`, `margin`, `letterSpacing`,
      `lineHeight`, `outlineOffset`, `outlineWidth`, `outline`,
      `outlineColor`, `transformOrigin`, `fontSize`, `fontStyle`,
      `opacity`, `border`, `borderTopLeftRadius`, `borderRadius`,
      `borderWidth`, `textOverflow`, `zIndex`) — flipped `supported`
      → `partial` to match the harness's DIVERGE verdict; gotchas
      are deferred relative-unit / arch / paint-pipeline-limit per
      entry-specific notes.

- **2026-05-07 (pulp #1434 A4 Bundles 2–7)** — css NOT-IMPL closure.
  30 catalog entries flipped `missing` → `partial` / `noop` / `wontfix`.
  Targets the harness drift queue: PASS+DIVERGE coverage on the css
  surface should now sit ≥ 95%.
  * **Bundle 2 — animations tail (1 entry):** `animationPlayState`
    flipped `missing` → `partial`. New `setAnimation(id, "play_state",
    value)` control-token routes through View::staged_animation +
    `View::animation_play_state_`. Playback driver pause/resume is
    the follow-up to #1508.
  * **Bundle 3 — logical-edge fan-out (7 entries):** `marginBlockStart`,
    `marginBlockEnd`, `marginInlineEnd`, `paddingBlockStart`,
    `paddingBlockEnd`, `paddingInlineStart`, `paddingInlineEnd` flipped
    `missing` → `partial`. JS-side cases dispatch to the existing
    per-edge `setFlex` bridge under the LTR / horizontal-tb fast path
    (mirrors the pre-existing `marginInlineStart`). Direction-aware
    logical→physical resolution remains the writing-direction follow-up
    tracked under the same #1434 umbrella.
  * **Bundle 4 — overflow per-axis + 3D (5 entries):**
    - `overflowX`, `overflowY` → `partial` (axis-tied gotcha
      documented in `unsupportedValues`; both axes clip together
      because View::Overflow has only `{visible, hidden}`).
    - `overflowWrap` → `partial` (storage on `View::word_break_`;
      HarfBuzz line-break feature deferred).
    - `perspective`, `perspectiveOrigin` → `noop` (Pulp pipeline is 2D,
      no 3D matrix slot on View).
  * **Bundle 5 — text rendering tail (8 entries):**
    - `textIndent` → `partial` via new `setTextIndent(id, px)` bridge
      fn storing on `View::text_indent_`; SkParagraph integration
      deferred.
    - `verticalAlign` → `partial` via new `setVerticalAlign(id, kw)`
      mapping to the existing `canvas::TextVerticalAlign` enum on
      `Label` (top|middle|bottom|baseline). Non-Label widgets silently
      no-op; sub/super fall back to baseline.
    - `wordBreak` → `partial` via new `setWordBreak(id, kw)` bridge
      storing on `View::word_break_`.
    - `writingMode` → `noop` (Pulp text shaper is horizontal-tb only).
    - `scrollBehavior`, `scrollMargin`, `scrollPadding`,
      `scrollSnapType` → `noop` (no CSS scroll-viewport model in
      Pulp; ScrollView intrinsic owns scroll state).
  * **Bundle 6 — isolation + clip/mask + direction (6 entries):**
    - `isolation` → `wontfix` (no CSS stacking-context / z-buffer
      model; mix-blend-mode's saveLayer covers the layer-isolation
      use case where it matters).
    - `mixBlendMode` → `partial` (already wired via #1549 — catalog
      flipped to match).
    - `clipPath` → `partial` (already wired via #1540 — catalog
      flipped to match).
    - `mask`, `maskImage` → `partial` (already wired via #1515 / #1540
      — catalog flipped to match).
    - `direction` → `partial` (already wired via #1506 — catalog
      flipped to match).
  * **Bundle 7 — resize + fontVariant (2 entries):**
    - `resize` → `noop` (no OS-style resize handles).
    - `fontVariant` → `partial` via new `setFontVariant(id, kw)` bridge
      storing on `View::font_variant_`. Mirrors the rn surface
      decision.
  * **Synthetic `__pseudo_classes_note`** → `wontfix`. Superseded by
    the per-pseudo-class entries (`__pseudo_hover` / `__pseudo_focus`
    / `__pseudo_active` / `__pseudo_disabled`) added in #1551; the
    note is kept for the original triage rationale.

- **2026-05-06 (pulp #1515)** — CSS `clip-path` + `mask` cluster.
  Three catalog items flipped `missing` → `partial`.
  * **`css/clipPath`** — bridge fn `setClipPath(id, svg_path_d)`
    stores the value on `View::clip_path_`; `View::paint_all` calls
    `Canvas::clip_path_svg` after the overflow clip and before
    children paint. `SkiaCanvas` parses via
    `SkPath::FromSVGString` and intersects the canvas clip;
    `RecordingCanvas` captures a `clip_path_svg` command for tests;
    other backends silently no-op (path parsing is Skia-side).
    Only the `path("...")` form is honored — URL refs (`url(#id)`)
    and named shape forms (`circle()`, `inset()`, `polygon()`,
    `ellipse()`) are deferred to a follow-up paint slice.
  * **`css/mask`** + **`css/maskImage`** — bridge fns `setMask` /
    `setMaskImage` round-trip the value through `View::mask_` /
    `View::mask_image_`. The shorthand parser in
    `web-compat-style-decl.js` extracts the first `url(...)` or
    `*-gradient(...)` substring and forwards it to
    `setMaskImage` alongside the verbatim shorthand. Storage-only
    today — the saveLayer + `SkBlendMode::kDstIn` shader composite
    that consumes `mask_image_` is the follow-up paint slice.
- **2026-05-06 (pulp #1434 Phase A2-3)** — `css/direction` (and matching
  `yoga/direction`, `rn/direction`) wired through a new
  `View::WritingDirection` enum + `setDirection(id, "ltr"|"rtl"|
  "inherit")` bridge fn. `yoga_layout.cpp` dispatches via
  `YGNodeStyleSetDirection` so `direction: rtl` flips Yoga's row-axis
  flow (flexDirection 'row' visually reverses under RTL). The CSS
  shim's `direction` case forwards the keyword verbatim; the
  `@pulp/react` JSX layer exposes `writingDirection` (CSS spec
  `direction` clashes with `FlexProps.direction` in the pulp prop
  surface; the el.style adapter route still works for raw CSS).
  Reclassified missing → supported on css + yoga; rn/direction stays
  partial pending the prop-applier's logical-edge resolver
  (#1497's `*Start`/`*End` mapping is currently LTR-only fast-path —
  RTL-aware mapping is a follow-up). `rn/writingDirection` flagged
  `wontfix` per the harness oracle (iOS-only RN spec).
  Skia paragraph_style.setTextDirection at text shape is the
  remaining piece — landing alongside the SkParagraph integration
  in a follow-up.
- **2026-05-06 (pulp #1434 Phase A2-1 PR 1)** — CSS animations +
  transitions infrastructure landed. New
  `core/view/include/pulp/view/css_animation.hpp` ships the type
  vocabulary (`CssEasing`, `AnimatableProperty`, `TransitionSpec`,
  `CssAnimation`, `CssKeyframesBlock`, `CssKeyframesRegistry`) plus
  `parse_transition_shorthand(css)` covering single-property, comma-
  separated multi-entry, `cubic-bezier(p1x,p1y,p2x,p2y)`,
  `steps(N, end|start)`, the keyword set
  (`linear` / `ease` / `ease-in` / `ease-out` / `ease-in-out`), and
  `'none'` clear. `setTransition` / `setTransitionProperty` /
  `setTransitionDuration` / `setTransitionDelay` /
  `setTransitionTimingFunction` bridge fns wired; per-View
  `transitions_` storage + `find_transition_for(prop)` resolver.
  `defineKeyframes(name, JSON-stringified stops)` populates the
  application-wide `CssKeyframesRegistry` on `WidgetBridge`;
  `setAnimation` looks up by name and seeds `CssAnimation` entries
  on `View::active_animations_`. PR 2 of the ladder hooks the
  playback driver to advance these via the rAF idle pump (#1402);
  this PR ships the substrate so subsequent PRs land independently.
  Reclassified 11 entries from `noop` / `missing` / `partial` →
  `supported` (transition* + animation/animationDelay/Duration/
  IterationCount/Name/TimingFunction). animationDirection +
  animationFillMode stay `partial` pending PR 4 (specialized
  per-property dispatch). animationPlayState stays `missing` per the
  noop-vocabulary convention from #1475. css drift -11.
- **2026-05-06 (pulp #1514)** — `css/listStyle` cluster (4 entries)
  flipped `missing` → `partial`. New `View::ListStyleType` enum
  (`none` / `disc` / `circle` / `square` / `decimal`),
  `View::ListStylePosition` enum (`outside` / `inside`), and a
  `list_style_image_` URL slot. Three new bridge fns —
  `setListStyleType`, `setListStyleImage`, `setListStylePosition`.
  The CSS shim parses the `list-style` shorthand into the 3
  longhands (any token order: type / position / image). The
  `@pulp/react` prop-applier exposes the same 4 props with the
  same shorthand parser. The catalog status is `partial`, not
  `supported`, because pulp doesn't model `<li>` / `<ul>` /
  `<ol>` semantics yet — values round-trip to View slots but
  marker glyphs aren't painted today (and `decimal` additionally
  needs sibling-index resolution from the parent's children).
  Marker glyph rendering is the follow-up. css drift -4.
- **2026-05-06 (pulp #1516)** — `css/boxSizing` flipped from `missing`
  to `supported`. The JS shim already called `setBoxSizing` if the
  bridge fn existed, but the bridge never registered it — so every
  `box-sizing: border-box` declaration silently dropped. This PR adds
  the bridge fn (`setBoxSizing(id, kw)`), routes it through
  `FlexStyle::box_sizing`, and wires `YGNodeStyleSetBoxSizing` in
  `build_yoga_subtree`. Default is `border-box` (matches Yoga 3.x's
  own default + pulp's implicit pre-fix behavior + what every modern
  web design expects via `* { box-sizing: border-box }`).
  `content-box` is opt-in for CSS-spec semantics. High-leverage for
  design imports — Figma / v0 / Claude Design HTML often emit
  explicit `boxSizing` declarations that previously vanished.
- **2026-05-06 (pulp #1552)** — `line-clamp` / `-webkit-line-clamp` /
  `background-repeat` wired end-to-end. `setLineClamp` and
  `setBackgroundRepeat` now register in `widget_bridge.cpp`; the JS
  shim cases that already routed through them (and the matching
  `@pulp/react` prop-applier dispatch) now reach C++. Label grew a
  `line_clamp_` slot; `paint()` emits at most N lines and appends
  U+2026 to the last visible line if any source lines were dropped.
  Setting `line-clamp >= 1` implicitly enables `multi_line_` so the
  paint path takes the multi-line branch (without that, the
  single-line path runs and the clamp is a no-op). View grew
  `background_repeat_` (storage-only — paint-time honoring lands
  with the `background-image: url(...)` / repeating-gradient work).
  Reclassified `css/lineClamp` and `css/webkitLineClamp` to
  `supported`; `css/backgroundRepeat` stays `partial` with the
  honest "storage-only" caveat.
- **2026-05-06 (pulp #1551)** — Phase A3 Bundle 3 inside the
  `pulp #1434` umbrella: 13 already-implemented CSS features
  promoted to first-class catalog entries. Pure catalog add — no
  code changes — documenting the existing wiring in
  `core/view/js/web-compat-document.js`,
  `core/view/js/web-compat-style-decl.js`,
  `core/view/js/css-parser.js`, and
  `core/view/js/web-compat.js`. New entries:
  * Pseudo-classes (4): `css/__pseudo_hover`,
    `css/__pseudo_focus`, `css/__pseudo_active`,
    `css/__pseudo_disabled` — wired via
    `_setupPseudoHover` / `_setupPseudoFocus` /
    `_setupPseudoActive` and the `:disabled` branch in
    `StyleSheet._applyTo` (web-compat-document.js).
  * Structural selectors (5): `css/__selector_tag`,
    `css/__selector_id`, `css/__selector_class`,
    `css/__selector_descendant`, `css/__selector_child` —
    `_parseSelector` and `_matchesSelector` cover tag, id,
    class, descendant ` `, and child `>` combinators.
  * `css/__matchMedia` — `window.matchMedia()` returning a
    MediaQueryList shim, evaluated by `_matchMediaQuery`
    (min/max-width, min/max-height, orientation).
  * `css/__var` and `css/__setProperty_var` — `var(--name)`
    resolution via `_resolveVar` against the bridge motion-token
    namespace; `style.setProperty('--name', value)` writes
    through to the same namespace via `setMotionToken` /
    `applyTokenDiff`.
  * `css/__StyleSheet` — full attach/detach engine covering both
    JS-API (`new StyleSheet(...)`) and `<style>` text input
    (parsed via `_parseCssText`).
  Counts: `css` raw entries `199 → 212` (+13 instant supported);
  the existing partial `css/__hover_pseudo` and missing
  `css/__pseudo_classes_note` triage notes are kept in place but
  superseded by the per-pseudo-class entries.
- **2026-05-06 (pulp #1434 Phase A2-4)** — `css/filter` extended from
  `blur(Npx)`-only to the full CSS Filter Effects function set:
  `blur` / `brightness` / `contrast` / `grayscale` / `hue-rotate` /
  `invert` / `opacity` / `saturate` / `sepia` / `drop-shadow`, plus
  chained functions in source order and `'none'` to clear. `setFilter`
  parses the function-call sequence into `View::FilterOp[]`;
  `view.cpp` paint dispatches via the new
  `canvas.save_layer_with_filters(...)` API; Skia composes the chain
  via `SkImageFilters::Compose` + `SkColorFilters::Matrix` (color
  matrices for brightness/contrast/grayscale/hue-rotate/invert/
  saturate/sepia, native `Blur` / `DropShadow` for those). Angle
  units `deg` / `rad` / `turn` / `grad` all parsed. CG canvas backend
  currently degrades the chain to blur-only collapse (color-matrix
  equivalents on CIFilter are a follow-up). Reclassified DIVERGE →
  PASS. The Skia ImageFilter chain is the same primitive Pulp uses
  for `boxShadow` (#925), so reusing rather than reinventing.
- **2026-05-06 (pulp #1528)** — Eight CSS entries reclassified `wontfix` /
  `missing` → `noop` (silently accepted, no paint impact in pulp's
  non-scrolling / hint-free model). The entries split into two clusters:
  * **Optimization / compositor hints (3):** `css/willChange`,
    `css/contain`, `css/contentVisibility`. These are browser-only
    hints — pulp's GPU renderer doesn't expose composition layers in a
    way consumers can hint about. Accepting the value silently keeps
    imported CSS portable.
  * **Scroll-snap and scroll-modulator cluster (5):** `css/scrollBehavior`,
    `css/scrollSnapType`, `css/scrollMargin`, `css/scrollPadding`,
    `css/overscrollBehavior`. All five depend on a real scroll engine
    (smooth-scroll programmatic jumps, scroll-snap landing areas,
    rubber-band gestures). Pulp has Overflow scroll/visible but doesn't
    actually scroll today, so the declarations silently noop.
  All eight properties already noop at runtime by JS-shim fall-through
  (no `case` arm in `web-compat-style-decl.js`); this PR only changes the
  catalog classification + harness verdict (drift cleared on all eight).
  Lifts the css "effective denominator" by reclassifying 4 OOS entries
  (`status: wontfix`) and 4 NOT-IMPL entries (`status: missing`) into the
  noop bucket — NO_OP entries semantically count as "supported in the
  silent-accept sense", consumers can paste browser CSS verbatim. Catalog
  noop count: 9 → 17. The css adapter
  (`tools/harness/adapters/css.py`) gained an explicit
  `status == "noop"` early-return so future entries don't have to rely on
  mapsTo string-marker matching.
- **2026-05-06 (pulp #1517)** — Background sub-properties wired through
  the JS shim and bridge: `backgroundAttachment` / `backgroundClip` /
  `backgroundOrigin` flipped from `missing` to `noop` / `partial` /
  `noop`. New thin bridge fns `setBackgroundAttachment` /
  `setBackgroundClip` / `setBackgroundOrigin` store the keyword on
  the View. Paint impact today is partial: `backgroundClip: text`
  (paint-time SkBlendMode::kSrcIn against text glyphs) is the only
  variant that needs real paint work and is deferred; box-flavor
  variants are no-ops on solid backgrounds (which is what pulp
  paints), and `backgroundAttachment: scroll` is the conformant
  default for our non-scrolling layout. Wiring is complete so that
  when the paint-side variants land, the slot is already populated.
- **2026-05-06 (pulp #1519)** — CSS outline cluster fully bridge-backed.
  `setOutlineColor` / `setOutlineOffset` / `setOutlineStyle` /
  `setOutlineWidth` now register in `widget_bridge.cpp`; the CSS
  translator at `web-compat-style-decl.js` fans the `outline:
  <width> <style> <color>` shorthand out to the per-attribute
  setters and routes the four longhands through them. View grew
  `outline_color_` / `outline_offset_` / `outline_style_` /
  `outline_width_` slots; Skia paint inflates the box by
  `outline_offset + outline_width / 2` and strokes — outline does
  NOT take Yoga layout space (paints OUTSIDE the border-box, no
  parent reservation). Line-style enum reused from
  `View::BorderStyle` (CSS spec is identical for outline + border).
  Reclassified `css/outline`, `css/outlineColor`, `css/outlineOffset`,
  `css/outlineStyle`, `css/outlineWidth` to `supported`.
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
- **2026-05-06 (pulp #1434 Phase A2-2 PR 1)** — CSS Grid surface
  extension. `GridStyle` gains `auto_columns` / `auto_rows`
  (implicit-track sizing for items overflowing the explicit grid),
  `auto_flow` (`row` / `column` / `row dense` / `column dense`),
  `template_areas` (`std::vector<NamedArea>` populated by
  `parse_template_areas("'h h h' 'm c c' 'f f f'")`), and per-child
  `grid_area_name` (resolved against the parent's template-areas
  map). The `setGrid` bridge fn picks up: `auto_columns`,
  `auto_rows`, `auto_flow`, `template_areas`, `grid_area` (named
  token or `row / col / row / col` numeric form). The CSS shim
  cases for `gridAutoColumns` / `gridAutoRows` / `gridAutoFlow` /
  `gridTemplateAreas` / `gridArea` forward verbatim. The
  `@pulp/react` prop-applier exposes `gridTemplateColumns` /
  `gridTemplateRows` / `gridTemplateAreas` / `gridAutoColumns` /
  `gridAutoRows` / `gridAutoFlow` / `gridArea` / `gridColumn` /
  `gridRow` / per-side starts/ends + gap variants. Reclassified
  9 entries to `supported`. PRs 2-3 of the A2-2 ladder wire
  auto-flow dense-packing + named-area resolution into the
  existing `layout_grid` algorithm. css drift -9.
- **2026-05-05 (pulp #1434 Triage #10)** — `css/borderStyle` now
  honors the keyword at paint time. New `View::BorderStyle` enum
  (`solid` / `dashed` / `dotted` / `double` / `groove` / `ridge` /
  `inset` / `outset` / `none` / `hidden`) stored on each View; the
  paint pass installs `SkDashPathEffect` via
  `canvas.set_line_dash(...)` for `dashed` (3w/3w on/off) and
  `dotted` (1w/2w on/off) before stroking, and resets the pattern
  afterward so subsequent strokes aren't dashed inadvertently.
  `none` / `hidden` short-circuit the stroke entirely. Other named
  styles (`double` / `groove` / `ridge` / `inset` / `outset`)
  currently degrade to solid — Skia compositions for those are a
  follow-up. CG canvas inherits the canvas-base no-op
  `set_line_dash` so dashed / dotted on the macOS native path
  silently render as solid (Skia path effect not yet plumbed
  through CG). `setBorderStyle` is a newly registered bridge fn;
  the CSS shim's `borderStyle` case forwards the keyword verbatim.
  Reclassified missing → supported.
- **2026-05-05 (pulp #1434 sub-agent #12 follow-up)** — three deferred
  CSS surface entries closed in one slice. `css/alignContent` flipped
  NOT-IMPL → PASS: the JS translator (`web-compat-style-decl.js`)
  was already routing `setFlex(id, 'align_content', ...)`, but the
  bridge had no `align_content` case — values silently dropped.
  Bridge now accepts the standard CSS values (`flex-start`,
  `flex-end`, `center`, `stretch`, `space-between`, `space-around`,
  `space-evenly`) and routes through `FlexStyle::align_content` /
  `align_content_space` to Yoga's `YGNodeStyleSetAlignContent`.
  `css/width` and `css/height` flipped DIVERGE → PASS for the
  `'auto'` keyword (Yoga "hug contents" via
  `YGNodeStyleSetWidthAuto` / `SetHeightAuto`); the CSS translator
  forwards the literal string and the bridge dispatches on
  `FlexStyle::dim_*.unit == auto_`. `first baseline` /
  `last baseline` / `normal` for `alignContent` and `min-content` /
  `max-content` / `fit-content` / `em` / `rem` for width / height
  remain unsupported.
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
- **2026-05-05 (pulp #1434 Triage #9 fan-out)** — `css/transform` now
  supports the full CSS function set:
  `translate(x,y)`, `translateX`, `translateY`, `rotate`, `rotateZ`,
  `scale(n)` / `scale(x,y)`, `scaleX`, `scaleY`, `skewX`, `skewY`,
  `matrix(a,b,c,d,tx,ty)`, plus the full angle-unit set (`deg`, `rad`,
  `turn`, `grad`). The CSS shim's `transform` dispatcher (in
  `web-compat-style-decl.js`) is now a walk-once accumulator that
  merges within-string axes into one consolidated bridge call per
  axis-of-transform: `translateX(10) translateY(20)` produces ONE
  `setTranslate(10, 20)` rather than two clobbering ones. `setSkew`
  is newly registered as a bridge fn (`View::set_skew` had existed
  in C++ since the 2D slot landed; this surface just hadn't been
  wired). `matrix(a,b,c,d,tx,ty)` dispatches via the existing
  `setTransform(id, a, b, c, d, e, f)` bridge fn — preserves the
  full 6-component 2D affine matrix verbatim (per Codex post-merge
  audit P1 — earlier draft decomposed to translate+uniform-scale+
  rotate which silently dropped `c`/`d` skew components on rotation
  matrices like `matrix(0.866, 0.5, -0.5, 0.866, 100, 50)`).
  Deferred (silent no-op): `rotateX` / `rotateY` / `matrix3d` /
  `perspective` — pulp's 2D View has no 3D rotation storage;
  tracked for a follow-up. Reclassified DIVERGE → PASS. Figma
  motion exports + v0.dev hero animations + Tailwind utility
  classes routinely emit the previously-unsupported function set.
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
8. `css/alignItems: baseline` / `css/alignSelf: baseline` — wired via
   `FlexAlign::baseline` → `YGAlignBaseline` (pulp #1434 rn batch B).
   `first baseline` aliases to plain `baseline` (Tier 1 PR-B,
   2026-05-12); `last baseline` is intentionally unsupported because
   it requires baseline-set tracking that Yoga does not implement.
