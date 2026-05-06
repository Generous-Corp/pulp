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

## Counts (2026-05-04)

| Status | Count |
|--------|------:|
| supported | 28 |
| partial | 7 |
| missing | 18 |
| wontfix | 0 |

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

`yoga/direction` is `missing`. Pulp does not currently model RTL
layout — `direction: ltr | rtl | inherit` on the View has no effect.
Logical-property props (`marginStart`, `paddingEnd`, `start`, `end`,
`insetInlineStart`, etc.) all degrade to a single direction.
