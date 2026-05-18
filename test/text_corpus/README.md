# Multilingual Text Corpus

This directory contains the multilingual torture corpus that backs the
Slice 1.3 measurement-vs-paint parity harness in Pulp's font-subsystem v2
hardening plan. The corpus is a curated set of UTF-8 strings, each
chosen to exercise a specific shaping, bidi, clustering, fallback, or
sizing hazard that the v2 plan identified — including the U+2190..U+21FF
Chainer-fallback bug that motivated this whole subsystem rewrite.

Each entry in `corpus.json` carries a stable `id`, the raw `text`, one or
more `categories`, a BCP-47 `locale` hint, a UAX #29 grapheme-cluster
count, a minimum shaped-run count, and notes describing what the entry
stresses and what could break. The corpus is consumed by the parity
harness via `corpus.json`; the harness asserts that the measurement pass
and the paint pass agree on cluster boundaries, advance widths, ink
boxes, and bidi run breakdowns for every entry.

`SCHEMA_NOTES.md` documents the trade-offs made for v1.0 (notably the
UAX #29 vs visual-cluster ambiguity for Devanagari conjuncts and Thai
diacritics) and lists candidate schema extensions to consider before
freezing the format for Slice 1.3+.

## Categories

| Category              | Count | What it stresses |
|-----------------------|------:|------------------|
| `bidi`                |    10 | LTR/RTL paragraph layout, neutral-character level resolution, embedded LTR digits inside RTL, Arabic contextual shaping. |
| `cjk`                 |     9 | Han / kana / Hangul shaping; locale-aware face selection (zh-Hans vs ja-JP vs ko-KR); jamo-to-syllable combining. |
| `cluster`             |    26 | Cluster atomicity across emoji ZWJ chains, regional-indicator pairs, keycap sequences, Devanagari conjuncts, Korean jamo. |
| `combining-marks`     |    11 | Base+mark attachment, multi-mark stacking, NFC vs NFD parity, Vietnamese stacked diacritics. |
| `devanagari`          |     4 | Virama-driven conjuncts, reordered vowel marks, multi-conjunct words. |
| `emoji-zwj`           |     4 | Four-person family, couple-with-heart, skin-tone modifiers, profession ZWJ + skin tone. |
| `fallback-arrow`      |     8 | U+2190..U+21FF arrows that hit the Chainer fallback bug; covers single-direction, double-stroke, and vertical arrows. |
| `keycap`              |     4 | Digit / `#` + VS16 + U+20E3 keycap enclosure sequences. |
| `mixed-script`        |    10 | Multi-script lines forcing run segmentation across Latin / Han / Arabic / Hebrew / Greek / symbol ranges. |
| `regional-indicators` |     4 | Country-flag pairs (US, JP, IL) and consecutive flag pair (US+JP) for greedy-pairing bugs. |
| `small-size`          |    13 | Plugin-label rendering at 7px (CROSSOVER, MID / SIDE WIDTH, res↑, cutoff→, numeric readouts). |
| `thai`                |     3 | No-whitespace word boundaries (needs ICU dictionary later), stacked tone marks, sentence-level run. |
| `vietnamese`          |     5 | Stacked horn + dot-below / horn + tilde, NFC vs NFD pair, full-sentence diacritic stress. |

(A single entry can carry multiple categories; the totals above sum to
more than the 60 unique entries.)

## Format

See `corpus.json` for the canonical schema. The minimum fields per
entry are:

- `id` — kebab-case unique identifier
- `text` — raw UTF-8 string to shape
- `categories` — array of category tags from the table above
- `locale` — BCP-47 language tag (hints face selection)
- `expected_clusters` — UAX #29 grapheme-cluster count
- `expected_runs_at_least` — minimum shaped-run count after bidi + script analysis
- `notes` — what this entry stresses and what could break

The corpus is plain JSON with no external dependencies. Validate it
with:

```bash
python3 -c "import json; json.load(open('test/text_corpus/corpus.json'))"
```

To re-derive `expected_clusters` for new entries:

```bash
pip3 install --user --break-system-packages grapheme
python3 -c "import grapheme, json; \
  data = json.load(open('test/text_corpus/corpus.json')); \
  print('\n'.join(f\"{e['id']:<45} {grapheme.length(e['text'])}\" for e in data['entries']))"
```

See `SCHEMA_NOTES.md` for nuances when authoring new entries (especially
Devanagari and Thai, where UAX #29 and visual-cluster counts diverge).
