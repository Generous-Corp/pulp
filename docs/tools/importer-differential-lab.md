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
and an advisory promotion classification. The observer places the
panel-local native import, unmodified, at the authored-frame origin both
captures sign (`comparison.geometry.registration`) inside an observer-only
page node, so its render and layout are page-space like Chromium's. The
full-frame visual score and `comparison.geometry.score` compare the two in
page space. The registered scores move both sides back into the panel's own
frame: geometry by subtracting that origin from Chromium's boxes and from the
native layout, the authored-frame visual by cropping the frame from both PNGs
at the same page rect. For a panel with page around it, that node
carries the canvas colour Chromium paints, resolved from the verified DOM
snapshot by CSS background propagation (`comparison.page_background`). When
html and body are both transparent the canvas is the user agent's, and it
resolves (`source: ua-canvas`) to the colour the capture recorded for it: a
probe given the root's computed color-scheme and a `Canvas` background reads
back what Chromium resolves, after every frame is captured
(`documentCanvas` in the DOM snapshot). A colour the rule cannot resolve is
left unpainted and reported `unresolved`. A
panel that covers the whole page owns its canvas (`importer-owned`), so a
dropped document colour shows up as a residual instead of being supplied.
The render receipt records the placement it applied, and the lab rejects a
receipt whose placement differs from what it requested.
`comparison.visual.full_frame_classification` names a full-frame score below
0.995. `panel-translation` means the residual is exactly an unplaced panel's
offset, and it is the only class that excuses one.
`page-background-unresolved` means pixels outside the frame differ while the
page colour is `unresolved`, so the observer left the page unpainted.
`unexplained` covers everything else, including an outside-frame residual
under a resolved or importer-owned colour. The second and third classes both
block promotion.
`comparison.work_counters` judges the observer's own counters from the
digest-bound render receipt: a tree that outlives its teardown, a paint
mutation after the first frame that requested no repaint, or (counter schema
v2) a read-back that decoded, uploaded or rebuilt an image makes the fixture
ineligible. v1 receipts carry no resource counters and are judged without
them; the verdict says `resources: absent`. `integrity.*_snapshot_semantic_sha256` hashes each DOM snapshot
with only its capture-local CDP identifiers fixed (the frame ID, loader IDs,
and backend node IDs, which Chromium numbers in a run-dependent order), which
is the digest to compare across repeated runs. The receipt paths are relative to
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

`--input` walks the directory recursively but skips anything under a
`holdout/` directory, so iterating on the corpus root never scores held-out
panels; pass `--include-holdout`, or point `--input` at the holdout directory
itself, to score them. `compare` and `benchmark` take `--viewport
WIDTHxHEIGHT` for panels taller than the default 1280x800 (manifest fixtures
declare theirs). Every comparison reads PNGs, so the commands check for Pillow
once before running and name `tools/ci/install_visual_python_deps.sh` when it is
missing.

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

### Rescore a finished corpus offline

```bash
python3 tools/import-validation/importer_differential_lab.py rescore \
  --corpus /tmp/importer-differential/corpus \
  --output /tmp/importer-differential/rescore-a
```

`rescore` replays every completed fixture's stored lane outputs through the
same `compare_one`: replay stand-ins for the importer and observer copy each
lane's recorded IR, capture evidence, render, layout, and render receipt into
place, so every binding check, registration, and lens runs unchanged with no
browser or importer build. `rescore.json` holds, per fixture, the report with
process timings, observability timings, and the stand-in executables' digests
removed, a SHA-256 of that report in emitted key order, and the digest of every
overlay image. Two rescores of one corpus are byte-identical exactly when the
scoring code produces the same report, so `cmp` of two `rescore.json` files is
the proof that a scoring refactor changed nothing. Fixtures whose corpus run
failed have no stored report and are listed as skipped.

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

- `ua-default-form-chrome`: the native theme painted it and the native
  lowering has no skin for it (a select's arrow, a text field, a meter, a
  progress bar, the picker types); a support boundary;
- `ua-theme-skinned`: the native theme painted it, and the native lowering
  paints it with Chromium's own theme (a range, checkbox or radio in the
  `chromium_refresh` skin, a `<button>` or button-type input as a
  `ButtonSurface`); not a boundary, so its residual counts against the
  importer like any other;
- `ua-shadow-form-part` — CSS paints the box, but parts such as a range thumb,
  colour swatch, date fields, or a styled select's arrow come from user-agent
  internals the snapshot does not serialize; a support boundary;
- `author-styled` — the computed box and the control's value describe it, and
  the native lowering is expected to match;
- `appearance-unrecorded` / `theme-unresolved` — the capture predates the
  column or the verdict, or could not resolve it; reported as a hypothesis,
  never a boundary.

Each control carries its share of the scored foreground error (the registered
authored-frame crop when registration ran). The share is the error inside the
theme-painted control's box plus one pixel, so it includes anything Pulp draws
inside that box, such as a button label; read it as "error inside UA-painted
control boxes", not as an exact theme attribution. A boundary makes the fixture `browser-required` and
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
| P3 | Prove native correctness | **NOT PASSED — P3a/P3b/P3c COMPLETE; P3d OPEN FOR FORM CHROME** | Final cold corpus at `3450a14c8d` (Chrome 151.0.7922.47, DPR 2, one ARM64 host), with three-run determinism passing on all 16 completed fixtures: on the frozen 14-fixture in-scope denominator, 12 meet every gate (structure >=0.99, frame placement plus registered geometry, typography >=0.98, registered visual >=0.995). Two fail the visual gate: held-out `08-form-controls` (registered visual 0.762539) and `18-keyed-list-panel` (0.947144), with the residual located inside UA-theme-painted control boxes; that support boundary was stated after the held-out failure, so it does not redraw this denominator and may be proposed as an exclusion only for the next frozen manifest. Held-out 01, 03, and 06 pass every gate. Gate coverage over the 16 completed fixtures: structure 16/16, geometry 16/16, typography 14/16 (the excluded Canvas2D controls), registered visual 14/16. F2a lowers native-verdict ranges, checkboxes and radios onto the Chromium-refresh widget skins, painted in the captured CSS-edge-snapped box with the theme-replaced UA background and border dropped: `08-form-controls` goes from 0.762539 to 0.953763, with three-run determinism `pass` (remaining foreground error: themed `<button>` 0.976, range 0.024, checkbox 0). The themed `<button>` is now the whole remaining P3d gap on both 08 and 18 (18 unchanged at 0.947144); a Chromium-refresh push-button skin is planned to close it. Lowering a native-verdict `<button>` to `ButtonSurface` (Chromium's push-button paint, with the UA background and border dropped) takes `08-form-controls` to 0.998861 and `18-keyed-list-panel` to 0.998588, with three-run determinism `pass` on both. Both now pass every numeric gate and are classified `browser-required` only by the `ua-default-form-chrome` support boundary. The remaining 08 foreground error is the range (0.961) and the button (0.039). |
| P4 | Compare baked-native and retained typed-plan paths | **DONE — NO QUALIFYING WIN** | An isolated retained typed plan (experiment branch `plan/importer-next-20261021-p4`, commit `169d06f977`, not landed) matched a fresh rebuild at all 600 trace steps, at six Skia PNG checkpoints, and detected a planted dropped meter update. Across seven interleaved separate-process Release reps on the synthetic benchmark IR, update-plus-frame median was 0.0892 ms against baked-native 0.0825 ms (1.08x) and p95 0.2003 ms against 0.1620 ms (1.24x), versus the pre-registered <=0.80x; allocations were equal (269 per frame) and peak RSS was within noise (+32 KB). It beat only the full-rebuild control (about 4.5x on median), which does not qualify. Layout and paint, shared by every lane, dominate the frame. The host was contended (load about 45 on 28 cores), so absolute times are noisy, but the ratios are far from the criterion. Separately, `main` landed (PR 10640, merge `e6da6d9816`) IR-bound retained and rebuild benchmark lanes (`--ir`, `--frames`, slot identity and text-mismatch counters, teardown readback). On importer-produced held-out IR its retained lane builds no views, rebinds no slots and records no unmarked mutations, while the rebuild control rebinds every slot every frame, and teardown leaves 0 live views. Those are structural and lifetime results, and that record makes no wall-clock claim. Combined: the retained path is behaviourally sound and keeps identity, and it has no qualifying wall-time or resource win on the one host measured. |
| P5 | Decide promotion or deferment | **DONE — DEFERRED** | The retained plan does not meet the pre-registered performance criterion, so baked-native remains authoritative and the retained plan stays an isolated experiment branch. The generic benchmark instrumentation (p95, warmup exclusion, per-frame allocation counts, RSS high-water) is retained as infrastructure, alongside `main`'s IR-bound lanes and lifetime counters, which landed independently and stay as landed. |
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
has the declared size and that its panel root sits at the placement origin
the render receipt records. The
registered score is eligible for the P3 visual threshold;
the full-frame score and outside-frame residual are always reported so panel
placement, background, overflow, and shadow defects remain visible. Missing or
disagreeing frame metadata fails closed rather than silently mixing registered
and unregistered metrics.

Chromium reports a transformed element's bounding box after its transform and
every ancestor's, while the native layout snapshot reports untransformed View
boxes. Native lowering applies a lone `rotate(<n>deg)` as a rotation about the
View's centre (the importer folds the authored `transform-origin` into the box
position), so the lab compares the bounding box of each native box under the
composed rotations of the node and its ancestors. A transform that native
lowering does not rotate, such as a list or a non-degree unit, keeps its
layout box on both counts.

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
result do not qualify.

Measurement scope as run: every lane is warm (2 s of idle rendering, then 60
discarded warmup frames); a cold first-update comparison was not measured.
`rss_high_water_bytes` is the process-lifetime `ru_maxrss`, so it includes
startup materialization and plan compilation, and a difference of a few 16 KB
pages is within run-to-run noise. The allocation probe replaces global
`operator new` in the benchmark binary, so absolute frame and update times are
not comparable with benchmark JSON produced before that instrumentation;
ratios between lanes of one build are. `bridge_calls` is zero for every native lane and is not
a discriminating counter; paint commands are RecordingCanvas counts, not GPU
draw or resource measurements.

### Benchmark instrument v2

`pulp-design-import-bench` receipts are `pulp-design-import-benchmark-v2`.
What changed, and what each field can and cannot show:

- **Stages.** Each frame is lapped as `update` (the fixture's state change),
  `layout`, `record` (painting into the frame's canvas) and `submit`, on one
  clock. `stages.residual_ms_max` is the largest frame time the laps do not
  cover; `stages.consistent` is false when more than 5% of sampled frames
  leave over 50 us or 2% of the frame uncovered.
  Fixtures no longer lay out inside `update`; v1 receipts laid out twice per
  baked frame and are not comparable per stage.
- **Memory.** `memory.rss` and `memory.phys_footprint` are
  `{median,p99,peak}_bytes` plus `source` and `reason`; an unreadable probe is
  `null` with a reason, never 0. `--cycles N` (N >= 3) adds settled
  build/run/teardown cycles and a least-squares memory slope across them,
  excluding the first; `--plant-cycle-leak` must make that slope rise.
- **Host conditions.** Load average and Apple thermal state are sampled at
  every phase boundary. `host_conditions.confounded` is true when the
  one-minute load exceeds cores/4, a reading is unavailable, or thermal state
  is not nominal, and a confounded receipt cannot support a speed claim.
  `--plant-load` replaces the measured load to prove the classification.
- **Canvas.** `--canvas=skia-raster|skia-graphite` paints through SkiaCanvas,
  so decodes, uploads, paragraph builds and rasterization are inside the
  frame. Counter schema v2 reports `image_decodes`, `gpu_uploads`,
  `paragraph_builds`, `scene_cache.{records,replays}` and
  `image_cache.{hits,builds}` per phase as total, per-frame maximum, and
  frames after the first that did any. `steady_state_gate` fails when any
  frame after a phase's first decodes, uploads or rebuilds an image; the
  recording canvas reports `blind`. `--require-steady-state` exits 4 unless
  the gate passes, and `--plant-frame-decode` must make it fail.
- **Live lane on importer output.** `--lane=live --ir=<panel.ir.json>
  [--bundle=<behavior.js>]` mounts the DesignIR as generated bridge
  JavaScript, under the materialized runtime bundle when one is given, and
  writes every anchored text slot through `setText` plus one host frame tick
  per step.
- **Comparison.** `tools/scripts/design_import_benchmark.py --compare BASELINE
  CANDIDATE --reps N --mechanism COUNTER:down|up|zero --stage STAGE
  --bench-arg=...` runs the two lanes in ABAB order as separate processes,
  observes thermal state around every rep with
  `tools/scripts/p2_thermal_state.swift`, and reports `win` only when every
  criterion of the pre-registered speed bar holds; otherwise `work-moved`,
  `no-win`, `confounded`, `insufficient-reps` or `instrument-inconsistent`.

Example: judge a candidate lane against baked-native on the Skia raster lane,
over an importer-produced DesignIR, under the no-launch environment:

```bash
PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 PULP_TEST_MODE=1 PULP_INSPECTOR_NO_LAUNCH=1 \
python3 tools/scripts/design_import_benchmark.py --build-dir build \
  --compare baked-native live --reps 5 --stage frame \
  --mechanism image_decodes:down \
  --bench-arg=--frames=600 --bench-arg=--warmup-frames=60 \
  --bench-arg=--canvas=skia-raster --bench-arg=--ir=/path/panel.ir.json \
  --output-json /tmp/verdict.json
```

Every rep also runs `--cycles=3` unless a `--cycles` bench argument is given,
because the memory slope is part of the criterion. `--mechanism` names the
per-frame counter the candidate claims to move (a v1 counter such as
`layout_passes`, or a v2 resource such as `image_decodes` or
`scene_cache.replays`); without one the best verdict is `work-moved`. Run it
only on a host below a quarter load per core: elsewhere every rep is
`confounded` by construction. A single receipt shows where a frame goes:
`interactive.stages.*.median_ms` splits update, layout, record and submit, and
`interactive.counters.resources` shows per-frame decodes, uploads and
paragraph builds.

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

**Paint-oracle ruling (2026-10-21).** Under
`Emulation.setDeviceMetricsOverride` with a device scale factor of 2, which
both the production capture and the lab's Chromium reference use, Chromium
rounds font metrics and snaps text baselines to whole CSS pixels. Real Chrome
launched with `--force-device-scale-factor=2` and no override rounds to device
pixels and changes text layout itself: on a 364-case grid (13 families, 7
sizes, two weights and styles) native DPR produced half-CSS-pixel fragment
heights in 296 cases, while emulated DPR 2 and DPR 1 matched canvas font
metrics in all 364. The import is CSS-pixel canonical by design, because that
layout renders the same geometry at every display scale; a native-DPR capture
would bake one display's half-pixel metrics into the IR. The emulated PNG is
therefore the only Chromium reference whose layout matches what Pulp renders,
and it remains the visual oracle. Reproducing the captured layout's whole-CSS-
pixel baseline snap in captured text is the captured layout's own paint
semantics, not instrument fitting. The documented product limitation is that
on a HiDPI display imported text can sit up to one device pixel from where
native Chrome would lay out the same source. A native-DPR pixel comparison was
prototyped and abandoned, because it scores Pulp's render of one layout
against Chromium's render of another.

Captured text baselines come from metrics measured during capture: each text
run's ascent and descent are read from an `OffscreenCanvas` in an isolated
world under the capture's own emulation, recorded in `platform-fonts.json`,
and used when the captured box is exactly ascent plus descent tall. Runs whose
font the canvas cannot express exactly record no metrics, and lowering falls
back to the rounded typographic-ascent rule, which is known to be approximate
for faces Chromium adjusts (Times, Helvetica, Courier on macOS).

**Final adversarial review (2026-10-22).** Fable reran the controls against the
final artifacts: the 02 calibration scores 0.999421, a planted 41x41 magenta
mutation 0.9466, a blank render 0.5569, and a 2 px shift 0.9260, all below
0.995; geometry plants (+/-3 px on the farthest node, +1.2 px, -10% width, IR
origin, native `html` rect, and native root shifts) each fail the gate while a
+0.9 px plant passes inside the ruled tolerance. The ruling is that the branch
may land, stating P3 as not passed. Fixes informed by a held-out fixture, so
not held-out generalisation evidence:

- `08-form-controls`: the form-control value text, theme verdict, input-type
  normalisation, default submit/reset labels, masked-control handling, runtime
  manifest additions, the input-type table pin, and the support boundary itself;
- `01-basic-card` and `06-absolute-overlay`: the geometry gate correction (an
  instrument change, ruled separately).

`18-keyed-list-panel` and `11-typography` are challenging fixtures in the
20261020 manifest, so their fixes are in-sample. Only `03-grid-meter` gives an
uncontaminated generalisation signal, and it also passed before tuning.

Release blockers for any claim of native import correctness: form-control
chrome (08, 18); the IPC socket timeout tests, which are timing-sensitive under
load and fail on `main` independently of this work; and the masked-field secret
fix, whose own tests fail without it (24 pass, 2 fail when the masking is
neutered). Deferred items: the RTL caret in `text_x_for_byte`, non-rendered text
outside form controls keeping its value in the snapshot, and the native-DPR
text-layout deviation of up to one device pixel per line. Imported
right-to-left text never reaches the RTL letter-spacing branch, because
DesignIR carries no text direction. The paragraph path
now finds the system UI font through a manager that serves it at the exact
requested weight (see the `text-metrics` skill), and a transparent html/body
resolves its page background from the UA canvas colour the capture records.

**Parallel `main` record (2026-10-21 to 2026-10-22).** `main` landed an
independent version of this work in PR 10640 (merge `e6da6d9816`) and its
follow-ups, plus keyed anchors in PR 10658. Its record, kept here as written:
the observer places the panel-local import in page space rigidly, inside an
observer-only page node that carries the canvas colour resolved from the DOM
snapshot, and the receipt records that placement; captured text baselines
follow Chromium's run rect, `bottom - round(fDescent)`, checked with a
`vertical-align: baseline` probe across 9 faces at 11-32 px; `fill_text` lands
its baseline on `y`; the observer's render receipt carries work counters and a
teardown readback that gate eligibility; IR-bound retained and rebuild
benchmark lanes. Held-out page-space visual at that head: basic-card
0.991 -> 0.999986, css-variables 0.992 -> 1.000000, flex-controls 0.999421,
grid-meter 0.999954, with typography across the corpus 0.596 -> 0.807. Its
planted negatives (registration omitted, double translation, missing or wrong
page background, lowered threshold, leaked view, malformed counters, NaN
scores) were all blocked, and a thermo-nuclear review, three adversarial
reviews and an independent Fable check completed.

**Reconciliation of the two records (2026-10-22).** One implementation is kept
per fix:

- Captured baselines: the two rules contradicted each other. This branch split
  half-leading inside a rect taller than its content area, and `main` read the
  baseline off the bottom. A CDP probe on Chrome 151 at emulated DPR 2 settled
  it: 216 cases (Arial, Inter, Georgia, Verdana, Times, Helvetica, Courier,
  `serif` and `system-ui` at 11-32 px, line-height `normal`, `1.0`, `1.5` and
  `40px`). The text rect was exactly the canvas-measured ascent plus descent
  in all 216, so a line-height never makes it taller, and the baseline sat
  the measured descent above its bottom in all 216. The merged rule is
  `main`'s bottom rule. It still prefers this branch's measured `LineMetrics`
  and keeps the whole-CSS-px snap on the captured frame's grid
  (`captured_baseline_offset` / `captured_baseline`). This branch's
  half-leading assertions were rewritten to the measured rule. The probe
  cannot tell the two measured rules apart, so the fallback (no measured
  metrics) was scored on its own. It used the faces' CoreText fAscent and
  fDescent. `bottom - round(fDescent)` was exact in 192/192 cases (Inter is
  excluded because CoreText substitutes Helvetica for it). The old
  rounded-ascent/half-leading fallback was exact in 96/192, and up to 2.86 px
  high on Times, Helvetica, Courier and `serif`. The bottom rule won. Its
  negative control is a `descent-override: 40%` Arial at 16px: Chromium puts
  the baseline 14 px down a 20 px rect, and the fallback puts it at 17, so the
  fallback is approximate for faces whose browser descent differs from the
  file's, and a test pins that. Probe receipts:
  `/tmp/pulp-importer-team/reconcile/probe/` (`rows.csv`, `analyze.py`,
  `ct.csv`, `fallback.py`, `fallback-result.txt`, `override.csv`).
- `fill_text`: this branch's hinting-aware painted baseline was kept, because
  it is equal to `main`'s rounding whenever hinting is on. Both raster tests
  pass.
- Typographic metrics: `main`'s `typographic_ascent()` / `typographic_descent()`
  names were kept, and this branch's `font_ascent()` was folded into them.
- Settle digest scrub: `main`'s top-level IR `settle_rounds` exclusion was
  added to this branch's envelope-only `provenance.settle` rule.
- Semantic snapshot digest: `main`'s `*_snapshot_semantic_sha256` names were
  kept, with this branch's implementation, which also normalises `loaderId`.
- Keyed anchors: `main`'s implementation, with its mixed-sibling report, was
  kept.
- Benchmark: `main`'s IR-bound lanes, fixed-frame pacing, verification
  exclusion and teardown readback were kept. This branch's warmup exclusion,
  p95, per-frame allocation counts and RSS high-water were added on top.
- Page-space placement and geometry: `main`'s observer placement, page
  background, receipt placement check and work-counter gate were kept. This
  branch's frame-placement proof and registered geometry gate now read native
  checks moved to the page-space observer. The native root must sit at the
  DOM authored origin and `html` at the page origin, with nothing
  subtracted. Registered geometry moves both sides back by the placement
  origin, so per-node deltas equal the page-space ones and only the
  relative-error denominators differ. The registered visual crops both PNGs
  at the authored frame, and the panel-translation shift control still runs
  whenever the two crop boxes differ. Ruling (team lead, Fable concurring):
  geometry passes only when this branch's gate (frame placement, registered
  mean, 1.0 CSS px per node, coverage floor) and `main`'s page-space mean
  (`comparison.geometry.score >= 0.98`, now the gate's `page_space_mean`
  check) both pass. A geometry result with no evaluated gate fails closed.
  Merged-artifact controls on `06-absolute-overlay`, which has its origin at
  (20, 20): baseline passes. IR origin +3, native `html` rect +3 and native
  root +3 fail frame placement. +3, -3 and +1.2 px on the farthest node and
  -10% width fail the per-node bound. +0.9 px passes. Visual controls on `02`:
  calibration 0.999421, planted 41x41 magenta 0.947643, blank render
  0.556867, 2 px shift 0.874403, all below 0.995 except the calibration.
  Dropping `06`'s stage backdrop scores registered 0.951346, against 0.999905
  for an unmodified re-render. A dropped backdrop that equals the page colour
  stays masked by the observer page node, which is a known limit.

### Stop and continuation rules

Do not expand compiler or retained-plan production work when an instrument
fails its controls, a receipt is unauthenticated, a held-out fixture regresses,
or the baked-vs-retained comparison lacks a meaningful wall-clock/resource win.
Those outcomes are valid results: record the evidence and keep the current
baked-native path. Continue through the next phase when its exit evidence is
complete and the preceding phase remains green.
