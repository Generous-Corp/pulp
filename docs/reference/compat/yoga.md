# Yoga compat

Status of the Yoga-shaped layout surface in Pulp's `FlexStyle`
(`core/view/include/pulp/view/geometry.hpp`). This is the underlying
flex engine that both `css/*` and `rn/*` consumers ultimately lower
onto.

The authoritative inventory is `compat.json` (`yoga/*` prefix).

## Generation

Generated inventory last refreshed: **2026-05-04** against
`origin/main` at SHA `a5f4f5ac`. The behavior notes below are
hand-maintained against the current source.

Spec walk: [Yoga Layout — Styling](https://www.yogalayout.dev/docs/styling/),
cross-checked against [RN Layout Props](https://reactnative.dev/docs/layout-props)
which mirrors the upstream Yoga API.

## Current support summary

Harness status is tracked by `tools/harness/verifier`; catalog status is
tracked by the `yoga/*` entries in `compat.json`. Use those generated
sources for current counts. This page records the behavior-level
rationale that is useful when a compatibility result changes.

The main flex-surface gaps that previously existed now have concrete
bridge and Yoga wiring:

- `aspectRatio` is stored on `FlexStyle::aspect_ratio` and propagated
  with `YGNodeStyleSetAspectRatio`. The bridge accepts both
  `aspect_ratio` and `aspectRatio`; non-positive or non-finite values
  clear the slot, matching CSS `aspect-ratio: auto`.
- `flexDirection` accepts `row`, `column`, `row-reverse`, and
  `column-reverse`. Unknown values preserve the bridge's historical
  column fallback.
- `flexWrap` accepts `nowrap`, `wrap`, and `wrap-reverse`, backed by
  the tri-state `FlexWrap` enum and Yoga's wrap modes.
- `alignItems` and `alignSelf` support baseline alignment. `alignItems`
  also accepts `first baseline` as an alias for Yoga's default baseline
  set; `last baseline` remains unsupported because Yoga does not expose
  the baseline-set tracking needed to distinguish it.
- `alignContent` routes through `YGNodeStyleSetAlignContent`. Space
  distribution values live on `FlexStyle::AlignContentSpace` because
  they are valid for line distribution but not for `alignItems` or
  `alignSelf`.
- `width`, `height`, `flexBasis`, top/right/bottom/left offsets, and
  per-edge margins/padding preserve px and percent units across the
  JS bridge. Width, height, flex-basis, and margins also preserve
  `auto` where Yoga exposes a native auto API.
- Logical edges (`marginStart`, `marginEnd`, `paddingStart`,
  `paddingEnd`, `start`, and `end`) route to Yoga's start/end edges
  and flip with the node's writing direction.
- Borders are sent to Yoga through `YGNodeStyleSetBorder` so
  box-sizing and content-box math see the same border insets that the
  renderer paints. Percent border widths remain unsupported because
  CSS rejects them and Yoga has no percent-border setter.
- `overflow: scroll` is accepted at the layout layer and propagated to
  Yoga. Painting currently clips scroll like hidden; scrollbar UI is a
  separate feature.
- `flex` and `flexFlow` are JS-side shorthands. The DOM-lite style
  adapter expands `flex` into grow/shrink/basis and `flexFlow` into
  direction plus wrap before values reach the C++ bridge.
- React Native shorthand aliases such as `marginHorizontal`,
  `marginVertical`, `paddingHorizontal`, and `paddingVertical` decompose
  to the existing per-edge fields before reaching Yoga.

Values deliberately left unsupported should stay listed in
`unsupportedValues` when accepting them would silently misrender:

- `flexBasis: content` is part of the CSS flexbox surface, but Yoga does
  not model it.
- Padding `auto` and percent border widths are invalid for the relevant
  CSS properties.
- `justifyContent: left`, `right`, `normal`, and `stretch` require axis-
  or item-size semantics that are not equivalent to any direction-
  agnostic `FlexJustify` value.

## Direction (RTL) support

`FlexStyle::writing_direction` (`inherit` / `ltr` / `rtl`) drives
`YGNodeStyleSetDirection` per-node and the root's
`YGNodeCalculateLayout` direction argument. The six logical-edge
props (`marginStart` / `marginEnd` / `paddingStart` / `paddingEnd`
/ `start` / `end`) flip with the parent's writing direction —
verified end-to-end by the layout asserts in
`test/test_widget_bridge.cpp`. The CSS `inset-inline-*`
shorthands and bidi-aware text shaping are tracked separately.
