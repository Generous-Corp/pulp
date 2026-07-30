# Importer Differential Lab

The Importer Differential Lab is a development-only Chromium-oracle harness.
It measures whether Pulp's smaller native/static HTML importer is converging on
the evaluated result that Chromium sees. It does not replace Chromium, alter a
canonical import, or silently select the candidate output.

## Why this comparison has two reference surfaces

Browser-backed HTML currently lowers to a pixel-exact `faithful_capture`
DesignIR root. Its semantic and layout evidence remains in
`dom-snapshot.json`. The candidate importer produces a native DesignIR tree.
Comparing those two IR trees directly would therefore be misleading.

The lab instead compares:

| Layer | Chromium reference | Native candidate |
| --- | --- | --- |
| Structure | evaluated DOM snapshot | DesignIR tree |
| Geometry | DOM snapshot bounds | post-Yoga `--dump-layout` bounds |
| Typography | computed styles | DesignIR text styles |
| Visual | `browser.png` | candidate DesignIR rendered through Pulp/Skia |

The raw browser screenshot is retained as the external render reference. Both
the native import and its validation render are run in a dedicated candidate
directory.

## Run one comparison

Build `pulp-import-design`, then run:

```bash
python3 tools/import-validation/importer_differential_lab.py compare \
  --importer build/tools/import-design/pulp-import-design \
  --observer build/tools/import-design/pulp-design-ir-observe \
  --file test/fixtures/import-differential/02-flex-controls.html \
  --output /tmp/importer-differential/flex
```

The stable output protocol is:

```text
source/
browser/
  design.ir.json
  browser.png
  capture.json
  dom-snapshot.json
  semantic-report.json
  tokens.json
candidate/
  design.ir.json
  render.png
  layout.json
comparison/
  report.json
  summary.md
  structural-diff.json
  classifications.json
  overlays/
```

Every report has `schema: "pulp-importer-differential-report-v1"` and records
source hash, browser provenance, source recognition, dynamic blockers, timings,
layer scores, likely root causes with confidence, and an advisory promotion
classification.

## Analyze a corpus

```bash
python3 tools/import-validation/importer_differential_lab.py analyze-corpus \
  --importer build/tools/import-design/pulp-import-design \
  --observer build/tools/import-design/pulp-design-ir-observe \
  --input test/fixtures/import-differential \
  --output /tmp/importer-differential/corpus
```

For explicit first-invocation and repeat-run latency distributions:

```bash
python3 tools/import-validation/importer_differential_lab.py benchmark \
  --importer build/tools/import-design/pulp-import-design \
  --observer build/tools/import-design/pulp-design-ir-observe \
  --file test/fixtures/import-differential/02-flex-controls.html \
  --runs 5 \
  --output /tmp/importer-differential/benchmark
```

Corpus output aggregates promotion shares, average latency and fidelity,
zero-false-promotion tracking, and recurring gaps ranked by affected fixtures,
visual impact, and root-cause confidence.

The checked-in corpus covers static HTML, flex, grid, CSS variables,
pseudo-elements, absolute positioning, SVG, form controls, Canvas, runtime
state, typography, and media queries. It is a diagnostic seed corpus, not proof
of general web equivalence.

The initial 2026-07-30 calibration run completed all 12 fixtures:

| Metric | Result |
| --- | ---: |
| Browser import mean | 9,841.6 ms |
| Native import mean | 15.5 ms |
| Native observation/render mean | 69.4 ms |
| Structural score | 0.603 |
| Geometry score | 0.039 |
| Typography score | 0.000 |
| Foreground-sensitive visual score | 0.755 |
| Production native promotions | 0 (disabled) |

A separate four-run repeat benchmark measured browser p50 at 9,898 ms and
native total p50 at 79 ms, a 9,819 ms median opportunity. This is an
opportunity figure, not realizable product savings: none of these fixtures is
currently eligible for a production browser bypass.

## Promotion policy

The reporting classifications are:

- `native-with-browser-validation`
- `browser-required`
- `unsupported`

Production promotion is disabled in this slice. A run is only marked
`threshold_eligible` when it has all of:

- structural score at least 0.99;
- geometry score at least 0.98;
- typography score at least 0.98;
- visual score at least 0.995;
- no recognized dynamic blocker.

React/Vue/Svelte evaluation, Canvas, WebGL, portals, shadow DOM, asynchronous
initialization, conditional rendering, and animation are conservative blockers.
Even a threshold-eligible run remains `native-with-browser-validation`: a
same-run Chromium comparison cannot prove that a future candidate-only policy
would safely skip Chromium. The production importer continues to use Chromium
regardless of this report.

## Improving the importer

Use `report.json` rather than visual score alone:

1. Run the corpus and inspect `ranked_gaps`.
2. Select a frequent, high-impact cause with strong evidence.
3. Add or improve a generic parser/layout rule or a framework adapter.
4. Run focused fixtures first.
5. Run the full corpus and compare score and classification deltas.
6. Run the existing browser, HTML, Figma, and DesignIR regressions.

Do not add fixture-specific exceptions. A direct adapter should be based on
authored AST/import/component identity when those inputs are available; raw
classname matching is only a weak source-recognition signal.

## Future CLI integration

If corpus evidence shows the lab is useful, the production CLI can expose the
same protocol as:

```text
pulp import-design --file prototype.html --compare-importers
pulp import-design analyze-corpus --input fixtures --output analysis
```

That integration should remain an orchestration layer over isolated importer
transactions. It must never insert comparison side effects into
`browser_import_session` or canonical publication.
