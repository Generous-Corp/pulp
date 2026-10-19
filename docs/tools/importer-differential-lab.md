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

## Reset workstream plan and status (2026-10-18)

This section is the authoritative status for the importer optimization
investigation. Work in older branches is reference material only. New work
starts from the current `origin/main` head and must regenerate evidence rather
than carry forward old scores.

### Objective

Establish whether Pulp can import a bounded, responsive HTML/CSS interface into
its existing native DesignIR and Skia/Dawn renderer with evidence that is
correct, repeatable, and useful for later optimization. The investigation has
two independent outcomes:

1. prove Chromium-to-native correctness on challenging and held-out fixtures;
2. determine whether an incremental retained typed plan improves the existing
   baked-native path on the same workload.

The retained plan is an optimization inside the existing renderer. It is not a
second production UI architecture. Chromium remains the sole browser oracle;
WebKit and other browser engines are out of scope. Pulp and Vellum ownership
routing is checked for every implementation slice.

### Evidence contract

Before collecting promotion evidence, write down and freeze:

- an authenticated PNG receipt binding source hash, Chromium build, Pulp head,
  renderer/backend, viewport, DPR, font receipt, image dimensions, and PNG
  hashes for both sides;
- parity thresholds for structure (>=0.99), page-space geometry (>=0.98),
  typography (>=0.98), and authenticated authored-frame visual similarity
  (>=0.995). Full-frame visual similarity remains separately reported, with
  outside-frame residuals recorded rather than cropped away; worst-fixture
  results are always reported separately;
- the held-out fixture manifest and hashes before tuning begins;
- an equivalence oracle covering DesignIR, declared image tolerance, and the
  same scripted state and interaction trace;
- a pre-registered performance win: median and p95 update or frame time,
  allocation/bridge/draw counters, and peak resource ceilings on one host
  class. A lower work counter alone is not a wall-clock win.

Every claimed metric must have a planted-defect negative control and a
blank-render control. A fixture is not eligible for a claim until stage
provenance, source recognition, and deterministic three-run receipts pass.

### Phase status

| Phase | Purpose | Status | Exit evidence |
| --- | --- | --- | --- |
| P0 | Close existing acceptance debt | **DONE** | Attributable M1 ARM64 PR-head receipt recorded for root-canvas and provenance: https://github.com/Generous-Corp/pulp/actions/runs/38996107653/job/117289012852 and protected artifact https://github.com/Generous-Corp/pulp/actions/runs/38996107653/artifacts/12088036268. The later merge-group run reused the receipt and ran zero suites; it is not claimed as fresh execution. |
| P1 | Reset and reconcile the workstream | **DONE** | Fresh branch `plan/importer-reset-20261018` from `origin/main` `4cbf6f9a845f081fb5fc5fe6f21d758389e64fe0`; old `measure/fixture11-viewport-20261015` is reference-only because its lineage SHA mismatches its head. |
| P2 | Validate the evidence contract and instruments | **DONE** | 214 Python controls pass; three-run cold benchmark determinism is `pass`; authenticated Skia render receipts, DPR 2 checks, blank-render and planted-defect controls pass; frozen baseline manifest is `test/fixtures/import-differential/reset-manifest-20261018.json`. |
| P3 | Prove native correctness | **IN PROGRESS — GATE NOT MET** | Baseline corpus run completed 16/18 fixtures; mean full-frame visual 0.821 and worst full-frame visual 0.355. The three-run `02-flex-controls.html` receipt is deterministic with structure/geometry/typography 1.0 but full-frame visual 0.978, below 0.995. The registered authored-frame score is now reported beside the canonical full-frame score; held-out results still need a clean gate run after challenging-set fixes. |
| P4 | Compare baked-native and retained typed-plan paths | **NOT STARTED** | Interleaved Release runs with identical inputs, proxy counters, wall time, p95, peak RSS/GPU resource measurements, and direct benchmark peak assertion. |
| P5 | Decide promotion or deferment | **NOT STARTED** | Promote only if behavior is equivalent and the pre-registered performance threshold wins without violating resource ceilings; otherwise retain the infrastructure as experimental and keep baked-native authoritative. |
| P6 | Code-health follow-ups | **ADVISORY** | Narrow `DocumentCanvasPaint` exposure, unify duplicate snapshot parsing, split the oversized browser-capture test unit, and add the direct benchmark peak assertion. These do not substitute for P0-P5 evidence. |

Phase 0 residual decomposition, the 29/29 focused Python tests, the merged
root-canvas lowering change, and the merged benchmark peak-preservation change
are useful infrastructure. They do not prove pixel parity or a retained-plan
performance win.

### Fresh baseline evidence

The fresh worktree built `pulp-import-design` and `pulp-design-ir-observe` through
the governed Release build. The 18-fixture cold corpus produced 16 completed
reports and two explicit failures (`15-svg-shapes.html` native painter refusal
and `16-svg-invalid.html` Chromium source rejection). The aggregate report
tracks the worst fixtures instead of hiding them behind a mean:

- `18-keyed-list-panel.html`: visual 0.355, geometry 0.722
- `14-canvas-state.html`: visual 0.370, geometry 0.500
- `13-canvas-primitives.html`: visual 0.370, geometry 0.500
- `11-typography.html`: visual 0.510, geometry 1.000
- `04-css-variables.html`: visual 0.935, geometry 0.721

The three-run cold benchmark for `02-flex-controls.html` has determinism `pass`
with identical source, importer, observer, browser capture, browser PNG, native
PNG, native layout, font, and Skia render-receipt digests. Its visual score is
0.978, while structure, geometry, and typography are each 1.0. The benchmark
measured only an 8.5 ms repeat-run median difference; it is not a production
performance claim. Receipts and overlays are retained at:

- `/tmp/pulp-importer-p2-corpus-20261018-run1/report.json`
- `/tmp/pulp-importer-p2-flex-benchmark-20261018/benchmark.json`
- `/tmp/pulp-importer-p2-flex-20261018-run1/candidate/render.png`

These are development artifacts, not checked-in goldens.

A post-correction spot run on five challenging fixtures plus the basic card
kept both visual surfaces visible:

| Fixture | Full-frame visual | Registered visual | Outside-frame differing fraction |
| --- | ---: | ---: | ---: |
| `01-basic-card` | 0.967 | 0.918 | 0.914 |
| `04-css-variables` | 0.935 | 0.888 | 0.872 |
| `06-absolute-overlay` | 0.980 | 1.000 | 0.806 |
| `07-inline-svg` | 0.936 | 0.999 | 0.873 |
| `11-typography` (held-out) | 0.510 | 0.596 | 0.846 |
| `18-keyed-list-panel` (held-out) | 0.545 | 0.706 | 0.000 |

These are diagnostic spot runs, not a closed P3 corpus gate. The follow-up
18-keyed-list run includes the importer fix that maps Chromium's transparent
html/body document canvas to its default opaque white composite and accepts
valid repeated `::marker` layout rows. It removed the outside-frame background
residual, but the registered score remains 0.706, so keyed-list rendering is
still an unresolved correctness issue. Registration removes a viewport
placement penalty for some fixtures while leaving typography and content
differences visible; it is not a reason to lower the threshold.

The first full corpus rerun after that importer correction completed 16 of 18
fixtures and retained the two explicit failure controls. Mean full-frame visual
was 0.812 and mean registered authored-frame visual was 0.846 across the 16
completed fixtures. The held-out `18-keyed-list-panel` registered score was
0.706 and remained classified browser-required; held-out `11-typography` was
0.596. P3 therefore remains open. The aggregate receipt is
`/tmp/pulp-importer-p3-corpus-20261019/report.json`.

### P3 denominator and registration contract

The P3 denominator is frozen before tuning. In-scope fixtures are every valid
fixture except the two imperative Canvas2D controls
(`13-canvas-primitives`, `14-canvas-state`) and the two explicit failure
controls (`15-svg-shapes`, native painter refusal, and `16-svg-invalid`,
malformed source). That leaves 14 fixtures, including held-out
`11-typography` and `18-keyed-list-panel`. The excluded controls remain in
every corpus run with their explicit reason; they cannot be removed to improve
an aggregate.

The canonical visual score remains the full PNG comparison. When the capture
contains an authored frame, the lab also emits a registered authored-frame
score and overlays. The frame metadata is hash-bound through the capture
receipt; it is evidence metadata, not an independent attestation. Registration
requires the native layout snapshot to prove that the imported DesignIR root
has the declared size and the observer viewport root is at origin. The
registered score is eligible for the P3 visual threshold;
the full-frame score and outside-frame residual are always reported so panel
placement, background, overflow, and shadow defects remain visible. Missing or
disagreeing frame metadata fails closed rather than silently mixing registered
and unregistered metrics.

P3 passes only when every in-scope fixture has authenticated three-run receipts,
structure >=0.99, raw page-space geometry >=0.98, typography >=0.98, and
registered authored-frame visual >=0.995, with no held-out regression and a
worst-fixture report. A registered score cannot erase a full-frame or
outside-frame defect; those defects must be fixed or explicitly classified in
the fixture report before P3 can close.

### Landed context

- [PR 10475](https://github.com/Generous-Corp/pulp/pull/10475) merged as
  [8fd4babdf18a84546e077213f7d68c8de4b9ea2c](https://github.com/Generous-Corp/pulp/commit/8fd4babdf18a84546e077213f7d68c8de4b9ea2c): conservative root-canvas
  capture/lowering and evidence validation.
- [PR 10479](https://github.com/Generous-Corp/pulp/pull/10479) merged as
  [3fa4520607216084cfdaa36d101327d74ea87a9c](https://github.com/Generous-Corp/pulp/commit/3fa4520607216084cfdaa36d101327d74ea87a9c): benchmark rebuild control
  preserving meter `peak` state.
- [PR 10474](https://github.com/Generous-Corp/pulp/pull/10474) is closed as
  superseded and has no remaining work to merge.

### Review decisions

Fable agreed that a fresh workstream is sound and called out the required tightening: close the M1 acceptance debt first,
define authenticated parity and performance thresholds before measuring, freeze
the held-out split, and require negative controls. They also confirmed that the
existing renderer and Chromium-only oracle should remain unchanged while the
retained-plan hypothesis is tested.

### Stop and continuation rules

Do not expand compiler or retained-plan production work when an instrument
fails its controls, a receipt is unauthenticated, a held-out fixture regresses,
or the baked-vs-retained comparison lacks a meaningful wall-clock/resource win.
Those outcomes are valid results: record the evidence and keep the current
baked-native path. Continue through the next phase when its exit evidence is
complete and the preceding phase remains green.
