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

1. `yoga/alignContent` — multi-line cross-axis distribution. CSS shim
   parses; `setFlex` switch has no `align_content` branch.
2. `yoga/aspectRatio` — both shim layers parse; `FlexStyle` has no
   `aspect_ratio` field. Silently dropped at the C++ boundary.
3. `yoga/flexDirection` reverse modes — `FlexDirection` enum has only
   `{row, col}`; `row-reverse` and `column-reverse` silently fall
   through to `col`.
4. `yoga/flexWrap` `wrap-reverse` — `FlexStyle.flex_wrap` is a bool;
   `wrap-reverse` is inexpressible.
5. `yoga/alignItems` / `yoga/alignSelf` `baseline` — `FlexAlign` enum
   has no baseline variant.

## Direction (RTL) support

`yoga/direction` is `missing`. Pulp does not currently model RTL
layout — `direction: ltr | rtl | inherit` on the View has no effect.
Logical-property props (`marginStart`, `paddingEnd`, `start`, `end`,
`insetInlineStart`, etc.) all degrade to a single direction.
