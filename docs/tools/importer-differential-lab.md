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
- no recognized dynamic blocker;
- no support boundary.

### Form-control support boundary

Chromium paints a form control from CSS or from its native theme, and computed
`appearance` alone does not say which. With `appearance: none` the control is
an ordinary box. With any other appearance the theme paints it unless the
author styled the box: a button, button-type input, or progress bar with an
author background, border, or border radius is painted from CSS, and so is a
text field or textarea with any of those or a box-shadow, while all of them
still compute `appearance: auto`. A select in that state keeps a theme-drawn
arrow. Checkboxes, radios, ranges, and the other picker controls keep the theme
whatever the author sets. Because the rule depends on who declared a property
(an author value equal to the user-agent default still counts), the capture
records a per-control verdict from Chromium's matched-rule origins in the DOM
snapshot's `formControlTheme`, and the lab and the native lowering both read
that verdict rather than re-deriving it.

Where the theme really paints, matching it would mean copying the browser's
own control appearance, so the lab states the boundary instead of scoring it as
an importer failure. `comparison.form_controls` lists every laid-out control
with its `appearance`, the capture's `theme` verdict, and one status:

- `ua-default-form-chrome` — the native theme painted it; a support boundary;
- `ua-shadow-form-part` — CSS paints the box, but parts such as a range thumb,
  colour swatch, date fields, or a styled select's arrow come from user-agent
  internals the snapshot does not serialize; a support boundary;
- `author-styled` — the computed box and the control's value describe it, and
  the native lowering is expected to match;
- `appearance-unrecorded` / `theme-unresolved` — the capture predates the
  column or the verdict, or could not resolve it; reported as a hypothesis,
  never a boundary.

Each control carries its share of the scored foreground error (the registered
authored-frame crop when registration ran), so the report says how much of the
residual is theme. A boundary makes the fixture `browser-required` and
ineligible. It never removes the fixture from a denominator or changes any
score. When every numeric gate passes, the promotion carries
`boundary_overrides_passing_gates: true` and says so in its first reason and in
the summary, and the corpus report lists every reclassified fixture with its
boundary shares under `support_boundary_fixtures`.

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
- parity thresholds for structure (>=0.99), geometry (frame placement plus
  registered intra-panel geometry; see the P3 contract below),
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
| P3 | Prove native correctness | **IN PROGRESS — P3a/P3b/P3c COMPLETE; P3d OPEN** | The latest cold corpus completed 16/18 fixtures: mean full-frame visual 0.799, mean registered authored-frame visual 0.846, and gate coverage of 13/16 structure, 8/16 geometry, 14/16 typography, and 6/16 registered visual. Fresh P3a evidence distinguishes browser-assisted design-time lowering from browser-free Skia observation; fresh P3b evidence has a deterministic three-run `02-flex-controls.html` calibration at registered visual 0.999421 with a planted-negative score of 0.94715. P3c now has four frozen held-out fixtures with authenticated three-run receipts; the set fails correctness thresholds, so P3d generic importer fixes remain open. |
| P4 | Compare baked-native and retained typed-plan paths | **DEFERRED UNTIL P3a/P3b AND A PASSING P3 SUBSET** | Interleaved Release runs with identical inputs, proxy counters, wall time, p95, peak RSS/GPU resource measurements, and direct benchmark peak assertion. P4 is a native-to-native equivalence/performance check and may use only fixtures that pass the corrected P3 evidence gates; it must not become a second production UI architecture. |
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
| `01-basic-card` | 0.904 | 0.918 | 0.000 |
| `04-css-variables` | 0.889 | 0.888 | 0.000 |
| `06-absolute-overlay` | 0.976 | 1.000 | 0.000 |
| `07-inline-svg` | 0.890 | 0.999 | 0.000 |
| `11-typography` (held-out) | 0.596 | 0.596 | 0.000 |
| `18-keyed-list-panel` (held-out) | 0.545 | 0.706 | 0.000 |

These are diagnostic spot runs, not a closed P3 corpus gate. The follow-up
18-keyed-list run includes the importer fix that maps Chromium's transparent
html/body document canvas to its default opaque white composite and accepts
valid repeated `::marker` layout rows. It removed the outside-frame background
residual, but the registered score remains 0.706, so keyed-list rendering is
still an unresolved correctness issue. Registration removes a viewport
placement penalty for some fixtures while leaving typography and content
differences visible; it is not a reason to lower the threshold.

The full corpus rerun after the comparator and importer hardening completed 16
of 18 fixtures and retained the two explicit failure controls. Mean full-frame
visual was 0.799 and mean registered authored-frame visual was 0.846 across the
16 completed fixtures. Gate coverage was 13/16 structure, 8/16 geometry,
14/16 typography, and 6/16 registered visual. The held-out
`18-keyed-list-panel` registered score was 0.706 and remained classified
browser-required; held-out `11-typography` was 0.596. P3 therefore remains
open. The aggregate receipt is
`/tmp/pulp-importer-p3-corpus-20261019b/report.json`.

The follow-up typography run now carries the browser's opaque body background
through a transparent html canvas. Its full-frame and registered scores are
both 0.596 with zero outside-frame residual; the remaining error is text edge
rendering. The visual residual diagnostic places 84.6% of differing cropped
pixels in the edge band, with matching foreground masks and layout metrics.
Fable's adversarial review classifies the consistent approximately +3.65
device-pixel vertical ink-centroid displacement as a likely baseline/leading
placement defect rather than generic anti-aliasing noise. Box geometry and
computed typography scores are blind to this defect; the visual receipt is the
instrument that currently exposes it. Before more importer tuning, the plan
must complete the instrument-integrity, real disagreement-floor, and replacement
held-out gates recorded in
`docs/reports/importer-reset-20261019-handoff.md`.

### P3a/P3b calibration evidence (2026-10-20)

The current-origin workstream records two distinct pipeline boundaries. The
native lowering import is intentionally **browser-assisted at design time**:
its capture provenance is `chromium-cdp`, and its import command carries
`--native-panel-lowering`. The subsequent native runtime stage invokes only
`pulp-design-ir-observe --backend skia`; its render receipt reports
`backend_used: skia`, `ok: true`, and its command contains no browser token.
Therefore the approximately 9.5-second native-import timing is browser-assisted
source evaluation plus lowering, not browser-free runtime startup. Native
readback render time in the fresh run was 96 ms.

The three-run cold calibration for `02-flex-controls.html` is deterministic:
all authenticated source, browser, native capture, PNG, layout, font, and Skia
receipt digests agree. Each run scored structure 1.0, geometry 0.999999,
typography 1.0, and registered visual 0.999421. A planted 41x41-pixel magenta
mutation scored 0.94715, below the 0.995 visual gate. This establishes a
useful known-good/known-bad comparator control for this fixture class; it does
not establish P3 across the corpus or prove a production performance win.

### P3c replacement held-out evidence (2026-10-21)

The replacement held-out manifest is frozen and committed as
`test/fixtures/import-differential/reset-manifest-20261020.json`. It contains
`basic-card`, `grid-meter`, `absolute-overlay`, and `form-controls`, with source
hashes recorded before P3d importer tuning. Each fixture has an authenticated
three-run cold receipt with matching source, Chromium capture, native capture,
PNG, layout, font, and Skia receipt digests. The campaigns used Chrome
151.0.7922.47, DPR 2, the same ARM64 host, and the same importer and observer
hashes.

| Held-out fixture | Structure | Geometry | Typography | Full-frame visual | Registered visual | Determinism |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `basic-card` | 1.000000 | 0.675549 | 1.000000 | 0.904209 | 0.917983 | pass |
| `grid-meter` | 1.000000 | 1.000000 | 1.000000 | 0.999954 | 0.999954 | pass |
| `absolute-overlay` | 1.000000 | 0.738819 | 1.000000 | 0.976336 | 0.999905 | pass |
| `form-controls` | 1.000000 | 1.000000 | 1.000000 | 0.712497 | 0.712497 | pass |

The worst held-out registered visual score is `0.712497` on form controls.
`basic-card` and `absolute-overlay` scored low on raw page-space geometry
because their authored panels begin away from the panel-local native root;
under the corrected geometry contract below, raw page-space geometry is
reported but not gated, and their registered geometry is within tolerance, while their full-frame residuals remain visible
in the report. The registered visual gate passes only for `grid-meter` and
`absolute-overlay`; the held-out set therefore fails P3 correctness as a
whole. Repeat-run native-total median differences were approximately 42.0 ms,
48.5 ms, 8.0 ms, and 9.5 ms respectively. These are browser-assisted
design-time transactions, not browser-free runtime startup or a retained-plan
performance result.

One initial `grid-meter` campaign stopped on a provenance mismatch during its
third invocation. It was discarded as an incomplete receipt and rerun in a
fresh output directory; the replacement three-run receipt passed. No score
from the failed campaign is used.

P3c is **DONE**. P3d remains **OPEN**: intrinsic sizing and page geometry,
typography baseline/leading placement, keyed-list behavior, and form controls
require generic importer work followed by full-corpus and held-out reruns. P4
remains deferred until a passing P3 subset exists and a real retained
materializer can be compared with baked-native on identical traces.

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
structure >=0.99; frame placement |recorded origin - observed Chromium
origin| <= 0.5 CSS px, verified against the native layout; registered geometry
>=0.98 with every matched box within 1.0 CSS px on x, y, width, and height;
typography >=0.98; and registered
authored-frame visual >=0.995. Raw page-space geometry is reported, not gated, with no held-out regression and a
worst-fixture report. A registered score cannot erase a full-frame or
outside-frame defect; those defects must be fixed or explicitly classified in
the fixture report before P3 can close.

### P4 pre-registered comparison (2026-10-21)

Frozen before any retained-plan measurement. The production baseline is the
`baked-native` lane with direct typed setters; `baked-native-rebuild`
(full rematerialization) is reported as a control but beating it alone is not
a win. Lanes run in separate, interleaved Release processes on one ARM64 host
with warmup frames discarded and the cache state declared.

The retained plan qualifies only if all of these hold on the same trace:

- behavior equivalence at every step against a freshly rebuilt tree (anchor
  set, bounds, typed widget values, recorded paint-command hash) plus Skia PNG
  hashes at checkpoints, and a planted dropped-update control that fails;
- per-update (update plus frame) median and p95 wall time each at most 0.80x
  of `baked-native`;
- allocation count and peak RSS no higher than `baked-native`.

Lower layout, invalidation, or paint-command counters without the wall-time
result do not qualify. `bridge_calls` is zero for every native lane and is not
a discriminating counter; paint commands are RecordingCanvas counts, not GPU
draw or resource measurements.

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

**Geometry gate correction (2026-10-21).** Every low raw page-space geometry
score in the corpus was a pure whole-tree translation by the authored-frame
origin: the native root is panel-local by contract, and `relative_error`
divides by the reference coordinate, so the root row alone costs 0.5 for any
nonzero origin. Raw >=0.98 was therefore unattainable by construction, and the
raw metric failed its own negative control (a +3 px child shift toward the
page origin raised the score). The previous metadata agreement check was
circular because the IR root's authored-frame origin is copied from the same
Chromium envelope the native capture carries. Fable ruled for an explicit,
non-circular frame-placement check: Chromium dom-snapshot bounds of the
authored root, the IR-recorded origin, and the native layout's root and
`html` rects must agree within 0.5 CSS px. Registered intra-panel geometry
keeps its >=0.98 mean and gains a per-node 1.0 CSS px absolute bound, which
the raw metric never had (measured floor across the 16 completed fixtures:
at most 0.0038 px). The frame-placement proof binds the Chromium DOMSnapshot
authored-root box, the DesignIR recorded origin, and the native layout's own
root and `html` rects; the capture-envelope agreement check is still run but
not counted, because it compares two copies of the same envelope. The recorded
origin remains transcribed from the candidate's Chromium capture, so the check
proves faithful transcription and correct panel-relative native placement, not
that native layout reproduces the page margins outside the authored surface. Each part carries planted negative controls (origin
metadata shift, native `html` rect shift, +/-3 px on the farthest node, -10%
width). This supersedes the earlier decision to keep raw page-space geometry
canonical; raw remains reported in every receipt.

**Paint-oracle correction (2026-10-21).** Under
`Emulation.setDeviceMetricsOverride` with a device scale factor of 2, which
both the production capture and the lab's Chromium reference use, Chromium
snaps text baselines to whole CSS pixels (`2 * round(L)` device px). Real
Chrome launched with `--force-device-scale-factor=2` and no override snaps in
device pixels (`round(2L)`), which is what Pulp's painter does. A 48-run
sub-pixel baseline sweep shows the emulated line pitch alternating 80/82 device
px while native Chrome and Pulp step 81 at the quarter phases. The emulated
capture therefore remains the layout oracle (DOM snapshot, geometry,
typography), but it is a miscalibrated paint reference for text. Fable ruled
against snapping native text to whole CSS pixels to match it, because that
would fit the instrument while moving the product away from real HiDPI
output. The visual gate is moving to a native-DPR Chromium reference, with the
emulated paint score still reported; visual scores taken against the emulated
reference are not comparable with native-reference scores.

### Stop and continuation rules

Do not expand compiler or retained-plan production work when an instrument
fails its controls, a receipt is unauthenticated, a held-out fixture regresses,
or the baked-vs-retained comparison lacks a meaningful wall-clock/resource win.
Those outcomes are valid results: record the evidence and keep the current
baked-native path. Continue through the next phase when its exit evidence is
complete and the preceding phase remains green.
