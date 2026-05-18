# Corpus Schema Notes

Discoveries made while authoring `corpus.json` that the parity-harness
implementer should keep in mind, and candidate schema extensions to consider
before Slice 1.3 freezes the format.

## Cluster-count ambiguity (UAX #29 vs visual cluster)

The `expected_clusters` field is currently defined as the UAX #29 extended
grapheme cluster count, validated with the Python `grapheme` library. For
most scripts this matches the user-perceived character count. **It does
NOT match for two cases that are central to the v2 hardening plan:**

### Devanagari conjuncts

`क्ष` (ka + virama + ssa) renders as ONE conjunct glyph in any reasonable
Devanagari font, but UAX #29 reports it as 2 grapheme clusters (ka is its
own cluster; virama+ssa form a second cluster). Likewise `क्षत्रिय` is
reported as 5 clusters but reads as 3 visual syllables (kṣa, tri, ya).

The parity harness needs to test BOTH:

1. The UAX #29 cluster boundary (so cursor movement matches platform
   conventions like CFStringTokenizer / ICU).
2. The visual-cluster atomicity — a font-fallback boundary in the middle
   of `क्ष` is still a P0 bug, regardless of which UAX #29 cluster it
   falls in.

### Thai diacritics

`ภาษาไทย` renders as 5 visual syllables but UAX #29 segments it into 7
clusters, because the vowel sign U+0E32 and tone marks count as their own
clusters under UAX #29 (Thai marks are not Extend in UAX #29). The corpus
records the UAX #29 value; the harness should treat the visual cluster
as glued for selection.

### Candidate schema additions

To make this explicit, future revisions could add:

```json
"expected_clusters_uax29": 2,
"expected_visual_clusters": 1
```

This was deferred to keep the v1.0 schema minimal. If the harness needs
both, add these alongside `expected_clusters` rather than replacing it.

## Field candidates for v1.1

While writing entries, several fields would have been useful but were
omitted to keep v1.0 small:

- `expected_baseline_y` — y-coordinate of the baseline at a reference
  font size; would let the harness assert vertical-metric parity for
  Vietnamese stacked diacritics, Devanagari `i`-mark hangs, and Thai
  high-tone marks.
- `expected_width_px_at_size` — map `{size_px: width_px}` for sizes
  Pulp actually renders at (12, 14, 16, 7 for plugin labels). Useful for
  catching advance-width rounding bugs at 7px without re-shaping in the
  harness.
- `expected_ink_box` — `[x_min, y_min, x_max, y_max]` ink bounds; would
  let the zalgo entry assert vertical extent.
- `required_locale_face_id` — hint string ("notosans-cjk-sc", "notosans-cjk-jp")
  for locked-down face selection in CI. Without it we cannot assert that
  `cjk-han-unification-001` actually picked a different face than
  `cjk-han-unification-002` — only that the locale was passed through.
- `script_runs` — explicit list of `[start_byte, end_byte, script_code]`
  triples. The current `expected_runs_at_least` is too soft for tight
  parity assertion.

Recommendation: defer all of these until the Slice 1.3 harness is up and
running so the schema additions are driven by real assertion needs, not
speculation.

## NFC vs NFD pairing

The corpus deliberately includes both NFC (`é` = U+00E9) and NFD (`é` =
U+0065 U+0301) forms of "é", and both precomposed (U+1EF1) and decomposed
forms of Vietnamese `ự`. The harness should pair these by ID prefix
(e.g. `combining-marks-nfc-e-001` ↔ `combining-marks-nfd-e-001`) and
assert that:

- `measure(nfc).width == measure(nfd).width`
- `paint(nfc).ink_box == paint(nfd).ink_box`

Once this passes, we have evidence that the shaper's mark-attachment path
is stable regardless of input normalization.

## Locale-pair harness

`cjk-han-unification-001` (zh-Hans, "骨") and `cjk-han-unification-002`
(ja-JP, "骨") deliberately use the same codepoint with different locales.
The harness should pair these by suffix-stripped ID and assert that the
two paints produce VISIBLY DIFFERENT glyphs (e.g. nonzero pixel diff in
the ink rect). If they are pixel-identical, locale routing is broken.

## Edge cases

- `edge-empty-001` (empty string) and `edge-single-space-001` are present
  to catch boundary crashes in the measurement pipeline. The harness
  should run them but not include them in any "selection round-trip"
  assertions.
- `edge-bom-001` has a leading BOM (U+FEFF). UAX #29 counts it as 6
  clusters (BOM + 5 letters); some shapers drop it silently and report 5.
  Either is acceptable as long as measure and paint agree on the same
  count for the same string.
- `edge-zwnj-001` (Persian with ZWNJ U+200C) tests that an invisible
  joiner-suppressor still has a click-mappable byte offset.

## Adding new entries

When extending the corpus:

1. Use the same `category-subject-NNN` id pattern; keep ids
   kebab-case and three-digit zero-padded.
2. Compute `expected_clusters` with the `grapheme` Python library or
   ICU's `BreakIterator` in `UBRK_CHARACTER` mode. Do not eyeball it —
   the Devanagari and Thai cases burnt 10 minutes during v1.0 authoring.
3. Cite the codepoints in `notes` for any non-trivial sequence
   (ZWJ chains, RI pairs, NFD vs NFC). Reviewers should not have to
   `\X`-decode the text to verify the test intent.
4. Update `README.md`'s category-count table.
