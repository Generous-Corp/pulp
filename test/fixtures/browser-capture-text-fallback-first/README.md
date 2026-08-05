# browser-capture-text-fallback-first

`browser-capture-text-wrap` with **only `platform-fonts.json` rewritten**, so
every run lists a one-glyph fallback face (`LucidaGrande`, `glyph_count: 1`)
*before* the face that actually shaped its text.

That ordering is not invented. Chrome produces it: across the shipped corpus,
every run mixing a primary family with a fallback glyph lists the fallback
**first** — a Jost paragraph containing a single `→`, which Jost lacks, reports
`LucidaGrande` at `resolved[0]` with one glyph and `Jost-Regular` second with
the other eighty.

Reading `resolved[0]` therefore stored the face that drew one character as the
basis for the whole paragraph. Native resolution of Jost can never equal
`LucidaGrande`, so the captured line breaking was rejected on every render, the
run re-derived its own, and the panel showed text drawn over itself. One arrow
was enough, and arrows are everywhere in these UIs.

The reader now takes the face with the highest `glyph_count`. This fixture is
the negative case for that rule; `browser-capture-text-wrap` — identical but for
this file — is the positive one, which is what lets a test attribute the
difference to the ordering and nothing else.
