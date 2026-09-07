---
name: text-metrics
description: Baseline, half-leading, and font-face resolution for Label and captured (browser-imported) text — the arithmetic that decides where a glyph lands and how wide the box must be, plus the measure-vs-paint divergences that make text clip or sit low without any test going red.
---

# Text metrics: baselines, half-leading, and face resolution

This skill covers `core/view/src/widgets/label.cpp` and the captured-text path
that design import feeds it. It exists because four separate bugs in this area
all shared one shape: **the number was wrong but nothing failed**, because the
test asserted the same rule of thumb the code used.

## The one rule: half-leading has a single reference

CSS puts the baseline of a line box at:

```
baseline_y = line_box.y + (line_box.height - ink_height) / 2 + ascent
             where ink_height = ascent + descent
```

The surplus you divide **must be measured against the same ink you then descend
by**. Measuring the surplus against the em box (`font_size`) and then descending
by a real face ascent double-counts the difference between those two references,
and paints every line low by exactly that gap.

Worked example, the bug that shipped:

| quantity | value |
|---|---|
| captured line box height | 15.0 |
| `font_size` | 12.0 |
| Inter's real ascent at 12px | 13.294921875 (≈1.108 em) |
| Inter's real ink (ascent+descent) at 12px | 17.162109 (≈1.43 em) |
| **buggy** `max(0,(15−12)/2) + 13.2949` | **14.794922** |
| **correct** `(15−17.1621)/2 + 13.2949` | **12.213867** |

**Negative half-leading is legal and must not be clamped.** A face whose ink
exceeds the captured line box has negative leading, and CSS lets the glyphs
overflow the box evenly, top and bottom. Clamping at 0 silently reintroduces the
line-height dependence the captured branch exists to remove — the 15px box above
carries −1.081 of leading, and clamping it is precisely the old bug.

`1.5 + 0.85 * font_size` is the rule of thumb this replaced. If you see `0.85`
anywhere near a baseline, it is measuring an em box while descending by a real
ascent. It is wrong even when the test agrees with it.

## Measure and paint must resolve the SAME font family

Every metric entry point — `intrinsic_width()`, `intrinsic_height()`,
`measured_height()`, `baseline_offset()`, `resolve_text_style()` — walks
`own → inherited → "Inter"`. `Label::paint()` did not: it resolved
`own → "Inter"`, skipping the inherited step. Anything that inherited its face
was therefore **measured in one font and drawn in another**.

Consequence, and why it is nearly invisible: the box is sized correctly for the
inherited face, so a wider painted face simply overflows and clips. It clips
*visibly* only where the element has no slack.

- A bare `<span>` is sized to exactly its text — zero slack — so it clips on
  screen. `SNAPSHOT` at JetBrains Mono 10px is `8 × 6.0 = 48.0` exactly; paint
  used proportional Inter, whose caps are wider, and the tail vanished.
- The same element inside a button with 8–11px padding absorbs the mismatch and
  is *silently wrong* rather than visibly clipped. Do not read "only one label
  clips" as "only one label is broken."

Fix shape: one `effective_font_family()` helper, used by paint and every metric,
so the walk cannot drift again.

## A Label with element children does not paint at its own origin

A `Label` that carries both text and element children is a flex container whose
own text is an **anonymous inline box** — a real item on the flex line, sized and
positioned by the layout pass alongside the element children. Painting that text
at the Label's content origin (`bounds()`) instead of at the box layout reserved
for it puts the text and the first child at the same coordinates, and they
overlap.

So the paint path has to take the text box as a parameter rather than read
`bounds()`. Every `bounds().width` / `bounds().height` inside the text painter is
a latent instance of this bug: wrap width, alignment origin, the `x` for centre
and right alignment, the available width for ellipsis, and the vertical baseline
arithmetic all mean *the text box*, not the widget. A Label with no element
children has a text box equal to its bounds, which is why the wrong version looks
correct in every fixture that does not mix text and children.

The related layout-side fact, which is what makes this easy to misdiagnose:
`build_yoga_subtree` attaches a measure func only when `!has_managed_children`.
A Label with both text and element children therefore contributes **zero layout
weight for its own text** — the anonymous box gets no intrinsic size from the
shaper. A test that only asserts child geometry passes while the text is
unplaced, so assert the text box itself.

## Gotchas

- **Captured line boxes in tests are usually hand-authored fixtures, not real
  Chromium captures.** `single.text_line_boxes.push_back({0,0,53.6719f,15.0f,...})`
  is a literal. An expectation written beside it encodes whatever rule the author
  believed, so a green test proves self-consistency, not correctness. Before
  trusting one, back-solve it: does the expected number equal the CSS formula
  with the *real* face metrics, or does it equal a rule of thumb?
- **Font metrics here are deterministic across platforms.** Linux CI and local
  macOS produced `14.794921875` to the last digit. So a cross-platform
  disagreement in a text test is a real logic difference, never "the platforms
  resolve different fallback faces" — check that excuse before believing it.
  It also means `margin(0.01f)` is a safe tolerance; you do not need a loose one.
- **Attributed text keeps its own height source.** `single_line_text_height` is
  `has_attributed_ ? automatic_lh : single_line_ink_height`; changing the
  half-leading reference does not disturb the attributed path.
- Widths come from the captured `basis.width` in `capturedTextBindings`
  (`native-ui/materialized/runtime.js`), which records what Chromium actually
  measured per span: `basis.width`, `resolved_face`, `resolved_faces[]`, and the
  full `requested.*` block. That is the ground truth for "how wide should this
  text be" — not a re-measurement.

## How to verify a change here

A baseline change that does not move a number is not a fix. Measure before and
after, and state both.

```bash
tools/scripts/confirm_failure.sh \
  --file core/view/src/widgets/label.cpp \
  --break "<perl -0pi -e to restore the old formula>" \
  --build-dir build --target pulp-test-design-import \
  --test ./build/test/pulp-test-design-import
```

Do **not** hand-roll this with `cp`/`.bak` and `touch`: restoring a source within
the same filesystem second leaves make comparing equal mtimes, so the object is
judged current and the binary keeps the old code. A stale object during the
*break* step makes the control falsely pass, which reads as "my test does not
cover this" and sends you to rewrite a test that was already correct.

Suites that cover this surface: `pulp-test-design-import`,
`pulp-test-widgets-label`, `pulp-test-typography-inheritance`,
`pulp-test-widget-metrics`, `pulp-test-canvas-fonts`, `pulp-test-text-shaper`,
`pulp-test-bidi-text`. They are registered by **target** name in
`test/cmake/view_widget_bridge_tests.cmake` — grepping cmake for a test's
*filename* finds nothing and looks like the suite does not exist.
