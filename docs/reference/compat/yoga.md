# Yoga compat

Status of the Yoga-shaped layout surface in Pulp's `FlexStyle`
(`core/view/include/pulp/view/geometry.hpp`). This is the underlying
flex engine that both `css/*` and `rn/*` consumers ultimately lower
onto.

The authoritative inventory is `compat.json` (`yoga/*` prefix).

## Generation

Last refresh: **2026-05-04** against `origin/main` at SHA `a5f4f5ac`.

Spec walk: [Yoga Layout — Styling](https://www.yogalayout.dev/docs/styling/),
cross-checked against [RN Layout Props](https://reactnative.dev/docs/layout-props)
which mirrors the upstream Yoga API.

## Counts (2026-05-07 — DIVERGE→PASS sweep)

Harness verdict (per `tools/harness/verifier`):

| Verdict | Count |
|---------|------:|
| PASS    | 55 |
| DIVERGE | 0 |
| NO-OP   | 0 |
| NOT-IMPL | 0 |
| OOS     | 1 (`yoga/boxSizing` — pre-existing oracle-omission) |

Catalog status counts (informational — `compat.json` `status` field):

| Status | Count |
|--------|------:|
| supported | 36 |
| partial | 6 |
| missing | 12 |

pulp #1542 lifted the seven logical-edge / direction NOT-IMPLs
(`yoga/marginStart`, `yoga/marginEnd`, `yoga/paddingStart`,
`yoga/paddingEnd`, `yoga/start`, `yoga/end`, `yoga/direction`) from
`missing` → `supported`. The DIVERGE→PASS sweep (this update) closed
the remaining 14 value-coverage gaps on the surface.

## Major gaps (parser routes, FlexStyle has no field)

1. `yoga/aspectRatio` — both shim layers parse; `FlexStyle` has no
   `aspect_ratio` field. Silently dropped at the C++ boundary.
2. `yoga/flexDirection` reverse modes — `FlexDirection` enum has only
   `{row, col}`; `row-reverse` and `column-reverse` silently fall
   through to `col`.
3. `yoga/flexWrap` `wrap-reverse` — `FlexStyle.flex_wrap` is a bool;
   `wrap-reverse` is inexpressible.
4. `yoga/alignItems` / `yoga/alignSelf` `baseline` — `FlexAlign` enum
   has no baseline variant.

## Recent updates

- **2026-05-07 (DIVERGE→PASS sweep)** — closed all 14 yoga value-
  coverage gaps in one PR. Three categories:
    - **Spec-invalid values dropped from `unsupportedValues`** —
      `borderXxxWidth` had `%` listed (CSS rejects `border-width: <%>`
      as invalid; Yoga matches by not exposing a setBorderPercent API),
      `paddingStart` / `paddingEnd` had `auto` listed (CSS rejects
      `padding: auto`), `padding` had `%` listed despite the per-edge
      keys already wiring it, `margin` had `auto` listed despite the
      per-edge keys already wiring it. These were drift, not gaps.
    - **Real wiring** — `yoga/start` / `yoga/end` now route `'auto'`
      through `YGNodeStyleSetPositionAuto` via FlexStyle::dim_*.unit
      (the old code claimed Yoga had no position-auto API; it does).
      `yoga/flex` shorthand keyword expansion (`flex: auto` ≡ `1 1 auto`,
      `flex: none` ≡ `0 0 auto`, `flex: initial` ≡ `0 1 auto`) wired
      in `web-compat-style-decl.js` instead of NaN-zeroing flex_grow.
      `yoga/overflow` gained the third spec keyword `scroll` —
      `View::Overflow::scroll` enum value, bridge keyword acceptance,
      and Yoga propagation via `YGNodeStyleSetOverflow`. Paint
      clipping treats scroll like hidden (no scrollbar UI yet); the
      harness gap was about layout-side keyword acceptance.
    - **Out-of-scope flagged correctly** — `yoga/flexBasis` `content`
      removed from `unsupportedValues`; the CSS-3 `content` keyword
      isn't modeled by Yoga itself, parity with upstream, not a Pulp gap.
  Net: yoga PASS 41 → 55, DIVERGE 14 → 0, drift 14 → 1 (the only
  remaining drift is `yoga/boxSizing` which is unrelated, pre-existing
  oracle-omission).
- **2026-05-06 (pulp #1545)** — `yoga/flexBasis` promoted partial →
  supported. The percent / `auto` / px wiring was added in pulp #1434
  rn batch C (FlexStyle::dim_flex_basis routes to
  YGNodeStyleSetFlexBasis{,Percent,Auto}); re-verified in #1545 that
  YGNodeStyleSetFlexBasisPercent fires for `'NN%'` strings end-to-end
  through the bridge. The `content` keyword stays in
  `unsupportedValues` because Yoga itself does not expose it (parity
  with upstream, not a Pulp gap). The `yoga/boxSizing` catalog
  addition originally bundled into this PR was deferred — the entry
  is now a forward-dependency placeholder at status `missing`,
  pinned to pulp #1516 / PR #1538 (the PR that actually adds
  FlexStyle::box_sizing + YGNodeStyleSetBoxSizing wiring); the
  catalog will flip to `supported` automatically once #1538 merges.
- **2026-05-06 (pulp #1434 Phase A2-3)** — `yoga/direction` wired at
  the View layer. `View::WritingDirection` enum (ltr / rtl / auto_)
  propagated to Yoga via `YGNodeStyleSetDirection(node, YGDirectionLTR
  | RTL | Inherit)` in `yoga_layout.cpp`. RTL flips Yoga's row-axis
  flow — flexDirection 'row' visually reverses under RTL.
  Complementary to #1542's FlexStyle-layer wiring (see entry below).
- **2026-05-06 (pulp #1543)** — `yoga/borderWidth` plus the four per-edge
  variants (`yoga/borderTopWidth` / `borderRightWidth` / `borderBottomWidth`
  / `borderLeftWidth`) flipped `missing` → `supported`. Pulp's borders
  were already painted as a Skia stroke via `View::set_border_*`, but
  Yoga never knew about them — `YGNodeStyleSetBorder` was never called.
  With #1516 having wired box-sizing (default `border-box`), Yoga's
  content-box math needs the actual border insets to subtract from the
  declared dimensions; a 0-border view had its content area too large by
  `2 * border_width` and children leaked under the painted stroke.
  `core/view/src/yoga_layout.cpp` now calls `YGNodeStyleSetBorder(node,
  YGEdge*, w)` per-edge in `build_yoga_subtree`. Per-side override (set
  via `setBorderTopWidth(id, w)` etc., which routes to
  `View::set_border_top` / right / bottom / left) wins over the uniform
  `View::border_width()` fallback. `%` remains unsupported (Yoga has no
  `YGNodeStyleSetBorderPercent` — matches CSS spec: percent values are
  invalid for `border-width`). 5 yoga catalog flips. Refs umbrella #1434.
- **2026-05-06 (pulp #1544)** — pure catalog promotion: `yoga/flex` and
  `yoga/flexFlow` now claim `supported`. Both shorthands have been
  wired through `core/view/js/web-compat-style-decl.js` for some time
  (the `flex` case fans out to `setFlex(flex_grow|flex_shrink|flex_basis,
  ...)`; the `flexFlow` case tokenizes and dispatches
  `setFlex(direction, ...)` + `setFlex(flex_wrap, ...)`), but neither
  was listed in the yoga catalog. Mirrors the existing `css/flex` and
  `css/flexFlow` entries. supported count: 35 → 37.
- **2026-05-06 (pulp #1542)** — yoga logical-edge fan-out. The six
  logical edges (`marginStart` / `marginEnd` / `paddingStart` /
  `paddingEnd` / `start` / `end`) and the writing-direction prop
  (`yoga/direction`) now reach Yoga directly. `FlexStyle` gained six
  `Dimension` fields (`dim_margin_start` / `dim_margin_end` /
  `dim_padding_start` / `dim_padding_end` / `dim_start` / `dim_end`)
  plus a `WritingDirection { inherit, ltr, rtl }` enum.
  `yoga_layout.cpp` dispatches each logical edge through Yoga's
  `YGEdgeStart` / `YGEdgeEnd` and pipes the writing direction to
  `YGNodeStyleSetDirection` (per-node) and the root's
  `YGNodeCalculateLayout` direction argument. Reclassified
  NOT-IMPL → PASS for 7 entries (yoga drift_count: 18 → 11).
- **2026-05-05 (pulp #1434 Triage #14)** — `yoga/flexWrap` now claims
  `wrap-reverse` alongside `wrap` and `nowrap`. `FlexStyle::flex_wrap`
  was converted from `bool` to a tri-state `FlexWrap` enum
  (`no_wrap` / `wrap` / `wrap_reverse`); `yoga_layout.cpp` dispatches
  through `YGWrapNoWrap` / `YGWrapWrap` / `YGWrapWrapReverse`. CSS
  `flex-wrap: wrap-reverse` and the `flex-flow` shorthand
  (`row wrap-reverse` etc.) now route correctly.
- **2026-05-05 (pulp #1434 sub-agent #12 follow-up)** — three deferred
  yoga gaps closed in one slice. `yoga/alignContent` was previously
  labelled "MAJOR — unimplementable"; that was a misread. Yoga
  supports it natively via `YGNodeStyleSetAlignContent`; the only
  gap was a missing `FlexStyle::align_content` field plus a
  `setFlex(id, 'align_content', ...)` case on the bridge. Bridge
  accepts both bare (`start`/`end`) and prefixed (`flex-start`/
  `flex-end`) spellings plus the three space-* values
  (`space-between` / `space-around` / `space-evenly`). The space-*
  variants live on a sibling `FlexStyle::AlignContentSpace` enum
  because `FlexAlign` has no space variants — those don't make sense
  for `align_items` / `align_self`. `yoga/width` and `yoga/height`
  gained `'auto'` (Yoga "hug contents" via
  `YGNodeStyleSetWidthAuto` / `SetHeightAuto`) — Figma auto-layout,
  v0.dev intrinsic-sizing cards, and Claude Design responsive
  containers all emit this. The CSS translator (`web-compat-style-
  decl.js`) and `@pulp/react` prop-applier forward `'auto'` strings
  verbatim to the bridge; `FlexStyle::dim_width` / `dim_height` carry
  the unit; `yoga_layout.cpp` routes on `dim.unit == auto_`.
  Reclassified NOT-IMPL → PASS for `alignContent`; DIVERGE → PASS
  for `width` / `height` (the percent path was already PASS post-
  #1426).
- **2026-05-05 (pulp #1434 cross-surface mega-batch)** — per-edge
  `margin_*` and `padding_*` accept percent strings; margin also
  accepts `'auto'`. `FlexStyle` gains `dim_margin_{top,right,bottom,
  left}` and `dim_padding_{top,right,bottom,left}` `Dimension`
  fields; `yoga_layout.cpp` dispatches on `dim.unit` to
  `YGNodeStyleSetMargin{Percent,Auto}` /
  `YGNodeStyleSetPaddingPercent`. Yoga's padding has no `auto` API
  (margin only). Mirrors the dimension/percent pattern from
  pulp #1426 (width/height) and #1451 (top/right/bottom/left).
  Reclassified DIVERGE → PASS for the 8 per-edge entries
  (`yoga/marginTop` through `yoga/paddingLeft`) plus the 4 RN-style
  shorthand aliases (`yoga/marginHorizontal` etc.). Net: yoga
  drift_count -12, pass +12.
- **2026-05-05 (pulp #1434 batch 6)** — `yoga/top`, `yoga/right`,
  `yoga/bottom`, `yoga/left` now accept percentage values (`'50%'`).
  The CSS translator and `@pulp/react` prop-applier pass `'NN%'`
  strings verbatim to the bridge; `View::top_unit_` / `right_unit_` /
  `bottom_unit_` / `left_unit_` carry the unit; `yoga_layout.cpp`
  dispatches to Yoga's native `YGNodeStyleSetPositionPercent`. Mirrors
  the `FlexStyle::dim_width` pattern from pulp #1423 (PR #1426) for
  the View positional fields. Reclassified DIVERGE → PASS for all
  four entries (yoga drift_count: 23 → 19).
- **2026-05-05 (pulp #1434 batch 4)** — React Native shorthand aliases
  `yoga/marginHorizontal`, `yoga/marginVertical`, `yoga/paddingHorizontal`,
  `yoga/paddingVertical` now fan out to the existing per-edge FlexStyle
  fields (`margin_left + margin_right`, `margin_top + margin_bottom`,
  `padding_left + padding_right`, `padding_top + padding_bottom`). No
  new FlexStyle fields — both JS-side translators
  (`web-compat-style-decl.js` for the DOM-lite el.style adapter and
  `prop-applier.ts` for the @pulp/react JSX intrinsic) decompose the
  shorthand before reaching the bridge. Reclassified NOT-IMPL → DIVERGE
  (parity with `yoga/marginLeft` etc — `auto` remains unsupported on
  margin, `%` remains unsupported on padding). progress_pct 69.81% →
  77.36%.
- **2026-05-05 (pulp #1423)** — `yoga/width` and `yoga/height` now
  accept percentage values (`'100%'`, `'50%'`). The CSS translator
  passes `'NN%'` strings verbatim to the bridge; `FlexStyle.dim_width`
  / `dim_height` carry the unit; `yoga_layout.cpp` dispatches to
  Yoga's native `YGNodeStyleSetWidthPercent` / `HeightPercent`. `auto`
  remains unsupported (deferred). Drift cleared on both entries
  (drift_count: 23 → 21).
- **2026-05-05 (pulp #1420)** — `yoga/display` catalog claims `flex`
  and `none` cleanly; reclassified NOT-IMPL → PASS. Bonus:
  `inline-block` ≡ `block`, `inline-flex` ≡ `flex` in the CSS
  translator (matches RN + CSS spec for non-text-flowing formatting
  contexts).

## Direction (RTL) support

`yoga/direction` is `supported` as of pulp #1542.
`FlexStyle::writing_direction` (`inherit` / `ltr` / `rtl`) drives
`YGNodeStyleSetDirection` per-node and the root's
`YGNodeCalculateLayout` direction argument. The six logical-edge
props (`marginStart` / `marginEnd` / `paddingStart` / `paddingEnd`
/ `start` / `end`) flip with the parent's writing direction —
verified end-to-end by the layout asserts in
`test/test_widget_bridge.cpp [issue-1542]`. The CSS `inset-inline-*`
shorthands and bidi-aware text shaping are tracked separately.
