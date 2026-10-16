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

Cache state is caller supplied and is never inferred from elapsed time. Declare
it when the harness is run (`--cache-state cold|warm|unknown`). Each receipt
also uses the shared GPU observability vocabulary: TTFP (time to first present),
TTNI (time to native interactive), and IFNF (import to first validated native
frame). This lab does not measure TTNI: it records native import completion as
an explicitly named `ttni_proxy`. TTFP and TTNI are unverified, and IFNF
remains readback-only until a real native present, interaction readiness event,
and same-transaction fidelity gate are captured. The cache declaration is
scoped to this importer lab; Graphite, pipeline, and Dawn-device dimensions
are explicitly marked not applicable here, not treated as measured.

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
layer scores, likely root causes with confidence, an `observability` receipt,
and an opt-in `timing_budget` verdict,
and an advisory promotion classification. The receipt paths are relative to
each fixture output, so they can be copied under `/tmp/pulp-p0a-8458/receipts/`
without exposing machine-local paths.

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

The benchmark preserves the caller's `--cache-state` declaration on every
run; it does not infer `cold` or `warm` from run order. The `first_invocation`
and `repeat_runs` fields describe sequence position only. The legacy `cold`
and `warm` fields are populated only when every run was explicitly declared in
that state; otherwise they carry an `unverified` status and a reason. Run
separate campaigns when both cache states are needed. Each receipt records the
source, browser provenance, importer and observer hashes, host identity,
per-run report links, fidelity comparison, and the native Skia render receipt.
Browser and native processes still run sequentially within a trial, so their
timing difference remains diagnostic until a controlled, alternated protocol
is used.

Each benchmark now records a `determinism` receipt. It is `pass` only after at
least three runs agree on the source, browser/native capture, browser PNG,
native PNG, native layout, and native Skia render-receipt digests. One- or
two-run campaigns remain explicitly `unverified`; a three-run disagreement is
a failed receipt rather than a reason to average the images together.

Timing numbers become load-bearing only when a caller supplies a frozen budget
file. The keys are measured importer-lab timings in milliseconds; omitted keys
are not gated:

```json
{
  "native_total_ms": 100,
  "native_render_ms": 80
}
```

Pass it to `compare`, `analyze-corpus`, or `benchmark` with
`--timing-budget budget.json`. The command exits nonzero when any measured
value exceeds its caller-supplied budget and records the failing runs in the
receipt. No default is inferred from a host or from cache state, and this gate
does not promote a headless readback to TTFP, TTNI, or IFNF evidence.

Corpus output aggregates promotion shares, average latency and fidelity,
zero-false-promotion tracking, and recurring gaps ranked by affected fixtures,
visual impact, and root-cause confidence.

The checked-in corpus covers static HTML, flex, grid, CSS variables,
pseudo-elements, absolute positioning, SVG, form controls, Canvas, runtime
state, typography, and media queries. It is a diagnostic seed corpus, not proof
of general web equivalence.

## Gate 1 calibration controls

The differential lab now carries a strict visual calibration selftest for the
production visual threshold (`0.995`). It uses a synthetic panel only to pin
the measurement behavior: a small edge-only tonal change is retained as an
advisory renderer-variance candidate, while one-pixel placement, color,
missing-border, wrong-weight, and corner-radius mutations all fall below the
strict gate. This protects the comparator from becoming permissive while the
real renderer floor is being measured.

The selftest is not Chromium/Skia evidence. A real disagreement-floor campaign
must provide authenticated same-source browser and native PNGs, a three-run
identical receipt, viewport and DPR, font and backend provenance, and the
corresponding native Skia render receipt. Until that campaign exists, the raw
visual threshold remains authoritative and no renderer setting or promotion
policy should change.

The methodology is informed by Octane's retained-plan and longest-task
benchmarks: preserve the complete receipt and phase boundaries before treating
an aggregate timing as a product claim. Runtime hydration and replay work
remain outside Pulp's importer scope; Vellum owns the browser and GPU parity
oracle.

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
