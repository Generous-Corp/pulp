---
name: canvas-text
description: >
  Canvas2D text: the SkParagraph/SkFont path behind fill_text, stroke_text, and
  measure_text, and the caches in front of it. Read before adding a cache to
  that path, changing font resolution, or trusting a text-related profile — the
  font-invalidation model has two independent generation counters and a hot
  symbol here is often not a hot cost.
requires: []
---

# Canvas text path

`SkiaCanvas::fill_text` (core/canvas/src/skia_canvas_text.cpp) is the Canvas2D
text surface. It resolves an `SkFont` via `make_font`
(core/canvas/src/skia_canvas.cpp), then builds and paints an SkParagraph via
`make_paragraph`. `measure_text`, `stroke_text`, `fill_text_sdf`, and the
caret-x query all route through the same `make_paragraph`.

## There are TWO font-generation counters, and a cache needs both

This is the trap. Font state mutates through two independent paths with two
independent monotonic counters:

| mutation | counter |
|---|---|
| free `register_font()` / `register_font_file()` / `register_font_url()` | `font_registration_generation()` — bundled_fonts.cpp |
| `FontScope::register_font()` (Global / Plugin / View scopes) | `merged_generation_for(scope)` — font_scope.hpp |

Keying a cache on **either alone serves stale text**. `merged_generation_for()`
is documented as "the signal downstream caches use to evict stale entries" and
`font_registration_generation()` is documented as existing so "downstream
typeface caches (skia_canvas.cpp, text_shaper.cpp) can invalidate themselves" —
both descriptions are accurate and neither is sufficient. Key on the SUM; both
are monotonic so the sum changes whenever either does. `text_cache_generation()`
in skia_canvas.cpp does this.

The failure mode is nasty and silent: a font registered *after* first paint —
an async webfont, a design-import hot reload, an app that registers a
materialized face at startup — is ignored forever, and the text renders in the
fallback face for the life of the process. It looks like a font-resolution bug,
not a cache bug. `test_canvas_text_cache.cpp` has the regression test; it failed
against a `merged_generation_for()`-only key and passes against the sum.

## A hot symbol in a self-time table is not automatically a hot cost

`make_font` runs on every text draw and shows up prominently in a profile, which
reads as "we rebuild the font per draw — cache it." Caching it bought **2-4%,
inside noise**, because `FontResolver` already memoises the typeface; the
residual `make_font` work is just setting SkFont fields. The real cost was
`make_paragraph` (~33% inclusive), which caching cut by ~50% on the text path.

Measure the fix, not the symbol.

## A CSS family LIST is much more expensive than a single family

`get_cached_typeface` has two paths. A single family delegates straight to
`get_cached_typeface_single`. A comma-separated list ("JetBrains Mono,
monospace") splits the string, builds a `FontOptions` per entry, and does a
`getFamilyName` + two `tolower` copies per candidate to reject SkFontMgr's
null-match fallback — all allocating, all per draw.

Measured pre-cache on the same workload: **6.6 us/call for the family list vs
4.6 us/call for a single family.** An app that registers its face and sets ONE
family name gets that difference for free. Benchmarks that use a single family
will not reproduce an app's real text cost — this bit an earlier measurement
here, which showed a font cache as worthless because the benchmark took the
cheap path the app never takes.

## Anything caching a paragraph must handle the baked-in paint

`make_paragraph` folds `current_fill_paint()` into the paragraph's `TextStyle`
via `setForegroundPaint`. That paint can carry a gradient shader, a Canvas2D
drop shadow (`SkImageFilters::DropShadow`), a CSS `filter` chain, and a blend
mode — not just a colour. So:

- Colour belongs in any cache key, or the same string renders in a previous
  draw's colour.
- Do not try to hash an arbitrary `SkPaint`. The current cache only admits
  PLAIN paints (no shader / colour / image / mask filter / path effect, blend ==
  `kSrcOver`) and lets everything else build fresh. Gradient text and
  `ctx.filter` stay correct by construction rather than by hash fidelity.
- `Paragraph::updateForegroundPaint` exists and would allow a paint-independent
  cache, but it is marked **experimental** upstream and the bundled Skia ships
  headers only — the implementation cannot be audited. Not currently used.

## Layout width is constant; maxWidth is a transform

`layout()` is called at exactly one place with `SK_ScalarInfinity`, so layout
width does NOT belong in a cache key here. Canvas2D `fillText(..., maxWidth)` is
implemented in `fill_text_with_max_width` as a horizontal canvas *transform*
(`canvas_->scale(scale, 1.0f)`), not a re-layout. Verify this is still true
before assuming it.

Backing scale/DPI is likewise absent from the key: `make_paragraph` takes no
scale and the device matrix applies at paint time. That is believed correct but
is **not currently covered by a test**.

## TextShaper is not a substitute for SkParagraph here

CLAUDE.md describes `TextShaper` as measure-once-reflow-forever, which makes it
sound like the natural home for cached label text. It is not, for `fill_text`:
`TextShaper` returns measurement data (`ShapedSegment`, `PreparedText`,
`ShapedLayout` — segment widths and line fragments) and nothing paintable.
`fill_text`'s quality path needs `SkParagraph::paint` for cluster-aware emoji
fallback, bidi, kerning, ligatures, and OpenType features. The only
non-paragraph painter in the file is the per-glyph blob fallback, which the
source documents as having no kerning or ligatures. Routing labels through
`TextShaper` means reimplementing painting at lower quality.

## Cache eviction: LRU, not clear-on-overflow

A real UI frame mixes immutable labels with at least one value that changes
every frame (a readout, a counter, a timecode). The changing string inserts a
new entry per frame, so a clear-on-overflow policy periodically discards the
static entries that were actually earning their keep. Profiling caught exactly
this in a first implementation here. Use LRU.

## Kill switch

`PULP_TEXT_CACHE=0` disables both the SkFont and paragraph caches, restoring
build-per-draw. It is read once into a `static const bool`. Use it to A/B a perf
claim in a single binary (identical code, identical machine conditions) and to
bisect any suspected stale-text bug.

## Trace env vars on this path are read once — except one

`PULP_FILL_TEXT_TRACE` and `PULP_PARAGRAPH_FONT_TRACE` (skia_canvas_text.cpp)
and the two in text_shaper.cpp are hoisted to function-local statics: they sit
on the per-draw path and `getenv()` takes the libc environ lock, which showed up
at ~3% of non-idle main-thread time during a drag.

`PULP_TEXT_SHAPE_SERIAL` in text_run_planner.cpp is deliberately **read live**
and must stay that way — `test_canvas_fonts.cpp` toggles it with
`setenv`/`unsetenv` between assertions to exercise both the serial and parallel
shaping arms, so caching it would silently pin the first value and neuter that
coverage. It is not on the per-draw path.
