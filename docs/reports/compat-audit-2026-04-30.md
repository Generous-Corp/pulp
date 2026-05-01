# Compat Matrix Audit (issue #1027)

Generated: 2026-04-30
Branch: `audit/compat-1027-populate`
Reference SHA: `4a423c03` (origin/main)

## Scope

This audit walks three reference specs and cross-references against the four
Pulp implementation surfaces that consumers actually touch. The output is the
populated `compat.json` at the repo root and this gap report. It replaces the
"discover gaps reactively while testing Spectr" loop with a systematic,
spec-driven inventory.

**Reference specs**
1. **Yoga style props** — https://www.yogalayout.dev/docs/styling, cross-referenced
   against https://reactnative.dev/docs/layout-props (mirrors Yoga upstream).
2. **React Native View / Text / Transform style props** —
   https://reactnative.dev/docs/view-style-props,
   https://reactnative.dev/docs/text-style-props,
   https://reactnative.dev/docs/transforms.
3. **MDN CSS property reference** — scoped to ~150 most-used layout, box-model,
   typography, color, transform, flex/grid, opacity properties. Print, scroll-snap,
   `@media`, animation `@keyframes` block-level concerns intentionally treated
   as `wontfix` for a GPU plugin renderer.

**Implementation surfaces** (the source of truth for "what's actually
implemented today")
- `packages/pulp-react/src/prop-applier.ts` — the @pulp/react JSX-prop switch
  (~250 LOC).
- `packages/pulp-react/src/host-config.ts` + `intrinsics.ts` + `types.ts` — the
  React reconciler + intrinsic-element surface.
- `core/view/js/web-compat-style-decl.js` — the DOM-lite `el.style.X = …`
  adapter (~750 LOC).
- `core/view/src/widget_bridge.cpp` — the C++ bridge (251 `register_function`
  entries).
- `core/view/include/pulp/view/{view,geometry}.hpp` — the View widget +
  `FlexStyle` C++ surface that everything ultimately lowers to.

## Counts

Per `compat.json`:

| Category | Total | supported | partial | missing | wontfix |
|----------|------:|----------:|--------:|--------:|--------:|
| `css/`   |   194 |        70 |      30 |      63 |      31 |
| `rn/`    |   114 |        39 |       3 |      69 |       3 |
| `yoga/`  |    53 |        28 |       7 |      18 |       0 |
| **Total** | **361** |  **137** |  **40** |  **150** |  **34** |

`react/`, `html/`, `canvas2d/` are reserved for the second-pass audit (Suspense /
Concurrent / refs; Element / HTMLElement / ARIA; Canvas2D op coverage). See the
`_note` keys in those sections of `compat.json`.

## Top 20 highest-priority missing/partial props

These are the props a typical RN-style or HTML/CSS consumer would expect to
"just work" and that today either silently no-op or break in subtle ways. They
are the most-likely-to-be-hit gaps when users port real UIs to Pulp. Filing
order is judgmental, leaning toward layout-correctness over polish.

1. **`yoga/alignContent`** (missing) — multi-line flex cross-axis distribution
   has no effect; `setFlex` switch has no `align_content` branch.
2. **`yoga/aspectRatio`** (missing) — parser exists in CSS shim but
   `FlexStyle` has no field; silently dropped.
3. **`css/flexDirection` / `rn/flexDirection`** (partial) — only `row` and
   `column`; `row-reverse` and `column-reverse` silently fall through to
   `column`. RN convention uses `column`, Pulp uses `col` — direct breakage when
   porting RN code.
4. **`css/flexWrap` / `yoga/flexWrap`** (partial) — bool-only; `wrap-reverse`
   inexpressible.
5. **`css/animation`** + 8 sub-properties (partial / missing) —
   `setAnimation` is registered but the body is a TODO no-op
   (`widget_bridge.cpp:3208`). All `@keyframes` UIs fail silently.
6. **`css/transform`** (partial) — only `scale`, `rotate`, `translate`,
   `translateX`, `translateY` honored. `scale(x,y)`, `scaleX`, `scaleY`,
   `skewX`, `skewY`, `rotateX/Y/Z` all silently dropped.
7. **`rn/transform`** (missing) — `@pulp/react` has no `transform` prop at all;
   the RN array-of-objects shape isn't surfaced.
8. **`css/lineHeight` unitless multiplier** (partial) — `parseCSSLength` only
   accepts px; the `lineHeight: 1.5` form (the most common) is silently
   dropped.
9. **`css/fontFamily` comma-separated lists** (partial) — line 465 strips
   quotes and passes the entire `"JetBrains Mono", monospace` as one face
   name. Famous bug, tracked in #927/#932.
10. **`css/borderColor` / `css/borderRadius`** (supported but buggy) — both
    re-apply `setBorder(id, color, width, radius)` with `0` for the
    other-side fields, silently overwriting prior color/width/radius. Setting
    `borderColor` after `borderRadius` zeros out the radius.
11. **`css/outline` / `css/outlineWidth` / `css/outlineColor`** (missing) —
    `setOutline` is referenced but never registered. Affects accessibility focus
    rings.
12. **`css/textShadow`** (missing) — `setTextShadow` referenced but never
    registered.
13. **`css/wordBreak` / `css/overflowWrap` / `css/wordWrap`** (missing) —
    `setWordBreak` referenced but never registered. Long-content text wrap
    cannot be configured.
14. **`css/lineClamp` / `css/webkitLineClamp`** (missing) — `setLineClamp`
    referenced but never registered. Multi-line truncation impossible.
15. **`css/boxSizing`** (missing) — `setBoxSizing` referenced but never
    registered. CSS sizing semantics ambiguous; Yoga is content-box-equivalent
    by default.
16. **`css/backgroundSize` / `backgroundPosition` / `backgroundRepeat`**
    (missing) — three setters referenced but never registered. Tied to
    `backgroundImage: url()` which is also missing — blocks any image
    background work.
17. **`css/transition[Property|TimingFunction|Delay]`** (missing) — only
    `transitionDuration` survives; CSS transition semantics largely lost.
18. **`css/alignItems` / `alignSelf` baseline** (partial) — RN supports
    `baseline`; we don't (FlexAlign enum has no baseline variant).
19. **`rn/borderRadius` + `borderTopLeftRadius` (and 3 corner variants)**
    (missing) — `@pulp/react` has only the object-shaped `border={…}` prop;
    flat corner-radius props are unwired.
20. **`rn/flex` shorthand** (missing) — RN's `style={{flex: 1}}` is the most
    common pattern in tutorials. `@pulp/react` users must type
    `flexGrow={1} flexShrink={1} flexBasis={0}`.

## 10 most-surprising "we have it but it's broken in subtle ways" cases

These are the ones that look fine in props, lower to the bridge, and then
discreetly fail or behave differently than the spec. These are exactly the
class of bugs the user surfaces when running real UIs.

1. **`setBorderColor` zeros the radius**, `setBorderRadius` zeros the width.
   Both call into the same `setBorder(id, color, width, radius)` and pass `0`
   for the other args. Order of CSS application matters.
2. **`setAnimation` is a registered no-op stub** (line 3208 — the function body
   is `(void)id; (void)prop; …`). CSS `@keyframes` and JSX `animationName`
   props look like they work but silently animate nothing.
3. **`aspect-ratio` parser-without-implementation** — the JS adapter parses
   `"16/9"` and emits `setFlex(id, "aspect_ratio", 1.78)`, but the C++
   `setFlex` switch has no `aspect_ratio` branch. Looks like it works in JS
   logging; doesn't lay out.
4. **`align-content` parser-without-implementation** — same pattern as
   aspect-ratio. The JS adapter routes it; the C++ side discards it.
5. **`fontFamily: "JetBrains Mono", monospace`** is passed literally to Skia as
   one face name. Quote-stripping eats the inner quotes but the comma stays.
   Skia's `SkFontMgr` returns null typeface, falls back to system default —
   silent degradation.
6. **`flexDirection: "column"` (RN) silently falls through to `col` default**.
   The bridge accepts `"row"` and treats every other input as the default
   `col`. So `column` (RN) and `column-reverse` BOTH map to `col`. RN users
   notice nothing wrong until reverse-mode UIs flip.
7. **`overflow: "scroll"`** silently maps to `hidden`. Users assume scrolling
   container; get a clipped one.
8. **`position: "fixed"` and `"sticky"`** are accepted by `setPosition` but
   layout behavior may not differ from `"absolute"` — needs verification.
9. **`opacity: "50%"`** — the percentage suffix is stripped by `parseFloat`,
   yielding `50`, which is then clamped to `1` by the renderer. Looks fine
   visually; semantically wrong.
10. **`touchAction`** is parsed and stored on the JS element instance but never
    propagated to the C++ hit-test. Touch-action restrictions don't actually
    constrain pointer events.

## Tranches for child-issue filing

Six tranches of related missing/partial props grouped for one issue each. Each
tranche is bounded so it can be one PR's worth of work; the goal is to keep
each tranche cohesive (one design conversation, one set of bridge wiring, one
test file).

### Tranche A: Layout completeness — Yoga props missing in FlexStyle (CRITICAL)
Adds the layout fields that turn Pulp's flex implementation from "approximately
Yoga" into "actually Yoga". Required before serious RN-style ports work.

- `yoga/alignContent` + `css/alignContent` — `FlexStyle.align_content` field +
  setFlex branch + layout pass cross-axis distribution.
- `yoga/aspectRatio` + `css/aspectRatio` — `FlexStyle.aspect_ratio` field +
  setFlex branch + layout pass aspect honoring.
- `yoga/flexDirection` reverse modes — extend FlexDirection enum to
  `{row, row_reverse, column, column_reverse}`.
- `yoga/flexWrap` `wrap-reverse` — change FlexStyle.flex_wrap from `bool` to
  enum `{nowrap, wrap, wrap_reverse}`.
- `yoga/alignItems` / `alignSelf` `baseline` — extend FlexAlign enum.

### Tranche B: Bridge setters referenced-but-missing (silent no-ops)
Eight setters that `web-compat-style-decl.js` calls behind `typeof` guards but
that the bridge never registers. Each is a silent no-op today. One PR to
register all eight as functional setters (or document them as wontfix).

- `setOutline`, `setOutlineWidth`, `setOutlineColor`
- `setLineClamp` / `setWebkitLineClamp`
- `setBackgroundSize`, `setBackgroundPosition`, `setBackgroundRepeat`
- `setBoxSizing`
- `setWordBreak`
- `setTextShadow`

### Tranche C: @pulp/react parity with bridge — easy adds
Eleven props where the C++ bridge already supports the call but `prop-applier.ts`
+ `types.ts` don't surface a JSX-level prop. One PR to mechanically add each.

- `pointerEvents`, `cursor`, `userSelect`, `textTransform`, `transformOrigin`,
  `backfaceVisibility`, `filter`, `overflow`, `borderRadius`,
  per-corner border-radius (4 props), per-side `border*Color` (4 props),
  per-side `border*Width` (4 props).

### Tranche D: RN flat-shape interop (`flex` shorthand, `marginHorizontal`, etc.)
RN-style flat props that map to existing bridge support but require either a
shorthand expansion in prop-applier or an alias surface. Adding these makes
direct RN ports compile.

- `flex` (shorthand) → `flexGrow`/`flexShrink`/`flexBasis`.
- `marginHorizontal` → `marginLeft`+`marginRight`.
- `marginVertical` → `marginTop`+`marginBottom`.
- `paddingHorizontal` → `paddingLeft`+`paddingRight`.
- `paddingVertical` → `paddingTop`+`paddingBottom`.
- `borderWidth` (flat) → setBorderWidth.
- `borderColor` (flat) → setBorderColor.
- `borderStyle` — note as wontfix-or-stub (renderer always solid).
- `display` (`'none'`/`'flex'`) → setVisible.
- `aspectRatio` flat prop (depends on Tranche A).

### Tranche E: Transform completeness — transform function set + RN transform shape
Today's `transform` parser only honors 5 of CSS's 12 transform functions. RN's
transform is an array-of-objects which `@pulp/react` doesn't model at all. One
PR to bring the surface to parity.

- `transform: scaleX(n)`, `scaleY(n)`, `scale(x,y)`.
- `transform: rotateX(deg)`, `rotateY(deg)`, `rotateZ(deg)`.
- `transform: skewX(deg)`, `skewY(deg)`.
- `transform: matrix(a,b,c,d,e,f)` from a CSS string (today only via direct
  `setTransform` bridge call).
- `transform: perspective()` + `perspectiveOrigin`.
- `@pulp/react` `transform` prop accepting RN's array-of-objects shape.

### Tranche F: CSS string-value coverage — keywords, percentages, units
Many CSS string values silently parse to NaN/0 or fall through. This tranche
fixes the parsers, not the bridge fns themselves.

- `flex: auto | none | initial` keyword → expand correctly.
- `fontWeight: normal | bold | lighter | bolder` keyword → numeric mapping.
- `lineHeight: <unitless number>` → multiplier of fontSize (cache after font
  resolution).
- `width|height|min*|max*|margin*|padding*|top|right|bottom|left|gap` percentage
  values → expose `Dimension` API on `setFlex` instead of `double`.
- `borderRadius: <%>`.
- `transformOrigin: <px>` (today only `<%>` and keywords work).
- `gap: <row> <col>` two-value syntax.
- `boxShadow` multiple comma-separated shadows.
- `transition`/`animation` shorthand multi-property comma list.

### Tranche G (optional, smaller): CSS animation / transition wiring
Wire up the `setAnimation` stub. Either implement a minimal keyframe runner or
explicitly mark `css/animation*` as `wontfix-for-now` and document the migration
path (e.g. `pulp.animate` API instead).

- `setAnimation` body — store + drive a KeyframeAnimation per-View in the frame
  clock.
- `transitionProperty`, `transitionTimingFunction`, `transitionDelay` —
  add bridge setters and adapter branches.
- `defineKeyframes` — actually store keyframe data instead of acknowledging
  and discarding.

## Methodology notes for follow-up audits

- The `_note` keys in `react/`, `html/`, `canvas2d/` mark sections that need
  a second pass. The methodology below applies symmetrically.
- WebFetch the spec's full reference page; if the spec page is too prose-heavy,
  fall back to MDN's flat property index.
- Walk the spec list against the four implementation surfaces in this order:
  1. `core/view/include/pulp/view/{view,geometry}.hpp` — does the underlying
     C++ data model have a field?
  2. `core/view/src/widget_bridge.cpp` — is there a `register_function` and is
     the body more than a `(void)…` stub?
  3. `core/view/js/web-compat-style-decl.js` — does the DOM-lite adapter
     route the value through `parseCSSLength` / `parseCSSColor` correctly,
     or does it silently fall through?
  4. `packages/pulp-react/src/prop-applier.ts` + `types.ts` — is there a JSX
     prop that surfaces it to React consumers?
- Status `partial` means "lower-than-spec-completeness with no runtime error".
  Status `missing` means "silent no-op" — these are the ones that bite.
- Status `wontfix` is for things that don't make sense for a GPU plugin
  renderer (print media, scroll-snap, table layout, list-style, ::before
  generated content, browser-only optimization hints like `will-change`,
  `contain`, `content-visibility`).

The compat.json itself is a live document. When a child-tranche issue lands,
update the affected entries' `status` and append the test path to `tests`.
