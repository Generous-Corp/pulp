# Layout Model

Pulp's layout engine is [Yoga](https://www.yogalayout.dev/), Facebook's
cross-platform Flexbox implementation. Pulp supports the layout primitives
Yoga supports — **CSS Flexbox and CSS Grid** — and nothing else.

This is intentional, not aspirational.

## What Pulp supports

- **Flexbox** — `display: flex`, `flex-direction`, `flex-wrap`, `justify-content`,
  `align-items`, `align-content`, `align-self`, `flex-grow`, `flex-shrink`,
  `flex-basis`, `gap`, `order`, all variants.
- **Grid** — `display: grid`, `grid-template-columns/rows`, `grid-area`,
  `grid-column/row`, `grid-auto-flow`, named lines, gap. Track sizing
  supports fixed `px`, fractional `fr`, and `auto` today; `minmax()`
  and `repeat()` are NOT yet implemented (`GridStyle::parse_template`
  in `core/view/src/view.cpp`). Use explicit track lists for now.
- **Position** — `position: absolute | relative | static | fixed`, with
  `top`/`right`/`bottom`/`left`/`inset`. Logical-edge variants
  (`marginInlineStart`, `paddingBlockEnd`, `start`, `end`) flip with
  `direction: ltr | rtl`.
- **Sizing** — `width`/`height`/`min-*`/`max-*` in `px`, `%`, `auto`,
  `fr` (grid-only). `aspectRatio`. `boxSizing: border-box | content-box`.
- **Overflow** — `overflow: hidden | scroll | visible` plus
  `overflow-clip-margin` adjustments.
- **Border + outline** — `border-width` per-edge, `border-radius`, `outline-*`
  drawn outside the border box.
- **Modern web** — `transform`, `opacity`, `filter`, `clip-path`,
  `mix-blend-mode`, `box-shadow`, `text-shadow`, gradients, animations
  (frame-driven playback per `compat.json css/animation*`), transitions.
  `mask` / `mask-image` are partial today — the bridge stores values
  but paint-time mask compositing (`saveLayer` + `SkBlendMode::kDstIn`)
  is deferred (#1540). See `compat.json` `css/mask*` for current status.

## What Pulp does NOT support — by design

The following CSS layout primitives are out of scope and will not be added.
They're marked `wontfix` in `compat.json`.

- **Block flow** — `display: block | inline | inline-block`, normal flow
  with line boxes.
- **Tables** — `display: table | table-row | table-cell`, `border-collapse`,
  `border-spacing`, `caption-side`, `empty-cells`, `table-layout`.
- **Multi-column layout** — `columns`, `column-count`, `column-width`,
  `column-rule`, `break-inside`.
- **Floats** — `float`, `clear`. (Modern alternatives: flex/grid.)
- **Print** — `page-break-*`, `print-margin`, paginated layout.
- **List item primitives** — `display: list-item` with auto-generated
  markers. The `list-style*` family is partial: the bridge stores the
  values verbatim on the View (so a future paint pass can honor them),
  but marker glyph painting is the architectural blocker — see
  `compat.json css/listStyle*` (`partial-deferred-paint-time`). Pulp
  does not model `<li>`/`<ul>`/`<ol>` semantics today.
- **Vertical writing modes** — `writing-mode: vertical-rl | vertical-lr`.
  (Logical-edge properties work for RTL horizontal flow, but vertical
  text layout is not supported.)

## Why

1. **Yoga is the engine.** Pulp uses Yoga directly. Yoga doesn't implement
   block flow, table layout, multi-column, or floats. Adding those would
   require either running a different layout engine alongside Yoga (double
   maintenance) or reimplementing block/table/columns on top of Yoga
   (massive refactor — likely an architectural rewrite).

2. **React Native parity.** React Native is also flex-and-grid-only. Pulp's
   `@pulp/react` package and JS-bridge component model are intentionally
   RN-aligned. Adding non-RN layout primitives would diverge from the
   React component-tree contract.

3. **GPU-first render pipeline.** Pulp renders via Skia + Dawn (WebGPU).
   This pipeline favors a single tree-walk layout pass that produces a
   geometry buffer. Block flow with mixed inline children, table cell
   auto-sizing, multi-column balancing, float wrap, and paginated
   layout all require complex multi-pass sequential algorithms that
   don't compose cleanly with that pipeline.

4. **Audio plugin and cross-platform app target.** Pulp targets structured
   UI: knobs, faders, sliders, side panels, popovers, parameter grids.
   Newspaper-column layouts, complex `<table>` styling, magazine flow,
   print pagination — none of these are real Pulp use cases.

5. **Modern design tools output flex/grid.** Figma, Stitch, v0, Pencil all
   produce flex containers + CSS Grid as their primary layout primitives.
   None export `display: block`, tables, or floats as default structure.
   Tool alignment matters for Pulp's design-import workflow.

## What this means for design imports

When importing from Figma/Stitch/v0/Pencil into Pulp, the source design's
auto-layout (which is itself flex/grid-shaped) maps directly to Pulp
primitives. There is no impedance mismatch.

If a source design happens to use `<table>` or floats (rare in modern
tools), the import path will reject those nodes or rewrite them into
flex containers — depending on the importer. See `docs/reference/design-import.md`.

## What this means for compat coverage numbers

Pulp's CSS compat coverage maxes out around **~85% raw / ~100% on non-OOS
denominator**. The remaining gap is exactly the layout primitives listed
above. Pushing past 85% raw on the unfiltered catalog would require
implementing the wontfix items, which contradicts this design.

When you read a coverage report, the raw percentage and the non-OOS
percentage tell different stories: the non-OOS percentage is the honest
"what fraction of in-scope CSS does Pulp support" number.

## Honest tradeoff

Pulp will never be a general-purpose web browser. It IS the right shape
for cross-platform native UI. The fixed scope is what makes the framework
coherent, the runtime fast, and the design-import contract clean.

If you need block flow, tables, multi-column, or print layout, use a
WebView. Pulp ships a WebView widget for exactly this case.
