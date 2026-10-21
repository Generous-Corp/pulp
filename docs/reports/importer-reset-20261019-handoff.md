# Importer reset handoff (2026-10-19)

Status: **ACTIVE; P3c remains open.** P0, P1, and P2 are complete. P3a
pipeline-boundary evidence and the initial P3b comparator calibration are
complete on the current-origin workstream; P3c and corpus-wide P3 remain open.
P4 and P5 have not started. This document is the restart packet for the next
session.

## Objective

Determine, with authenticated evidence, whether bounded responsive HTML/CSS
can be imported into Pulp's existing DesignIR and Skia/Dawn renderer with
correct behavior, and only then determine whether a retained typed update plan
beats the existing baked-native path. Chromium is the sole browser oracle.
WebKit is out of scope. The retained plan is an optimization inside the
existing renderer, never a second production UI architecture.

## Current evidence

- Fresh branch: `plan/importer-reset-20261018`, based on `origin/main`.
- Python importer suite: 217 tests passed.
- Focused root-canvas C++ suite: 601 assertions passed.
- Cold corpus: 18 fixtures, 16 completed, 2 explicit controls failed as
  expected. Mean full-frame visual `0.799`; mean registered visual `0.846`.
- Gate coverage over completed reports: structure `13/16`, geometry `8/16`,
  typography `14/16`, registered visual `6/16`.
- Worst registered scores: Canvas2D controls `0.393` and `0.426` (excluded
  browser-required controls), typography `0.596`, keyed-list `0.706`, and
  form controls `0.712`.
- Typography has matching ink mass and pixel count but a consistent roughly
  `+3.65` device-pixel vertical centroid displacement; about `84.6%` of
  differing cropped pixels are in the edge band. Box geometry and computed
  typography scores therefore do not prove glyph placement.
- Only `02-flex-controls.html` currently has a deterministic three-run
  receipt. The corpus reports are single cold runs.
- The fresh three-run `02-flex-controls.html` calibration is deterministic at
  registered visual `0.999421`; a planted 41x41-pixel mutation scores `0.94715`.
- The native lane is browser-assisted during design-time lowering, while the
  separate Skia observer has no browser argument and emits a successful Skia
  receipt. Its 96 ms render time must not be conflated with the roughly 9.5 s
  browser-assisted lowering time.

## Adversarial review findings

Fable reviewed the plan and current receipts. The initial calibration confirms
the comparator can distinguish a known-good fixture from a clear planted
visual defect. Before any fixture-specific or generic importer tuning:

1. **P3a: instrument integrity (initial slice complete).** The report now
   distinguishes browser-assisted design-time lowering from the browser-free
   Skia observation stage and records the command/receipt evidence. The roughly
   9.5-second native-import timing is explicitly not a runtime performance
   claim.
2. **P3b: renderer disagreement floor (initial slice complete).** The fresh
   `02-flex-controls.html` three-run control is deterministic at 0.999421, and
   its planted negative scores 0.94715. Extend this calibration to additional
   fixture classes; do not lower the threshold during that work.
3. **P3c: held-out integrity.** The former effective held-out set is
   contaminated because typography and keyed-list findings guided recent
   fixes. Freeze and hash a replacement held-out set of at least four in-scope
   fixtures before further tuning, and record which fixtures guided each rule.
4. **P3d: correctness, only after P3a-c.** Address geometry/intrinsic sizing
   first, then typography baseline/leading placement, keyed-list behavior, and
   form controls. Treat `svg-shapes` as a known native painter gap, not as a
   malformed-source control; keep `svg-invalid` as the malformed-source
   control.

The review also found that the old phase row, held-out wording, and mixed
14-versus-16 denominators were misleading. The authoritative phase table and
this handoff now state the current values and prerequisite gates explicitly.

## Restart sequence

1. Run lineage and clean-state checks in a new worktree from the current
   branch or its successor.
2. Complete P3a, P3b, and P3c only; do not change importer rules or begin P4.
3. Stop and report if the candidate consults Chromium or if known-good native
   renders establish a visual floor below `0.995`; those outcomes require an
   explicit comparison-policy decision, not silent threshold relaxation.
4. If P3a-c pass, begin P3d with intrinsic sizing and rerun the replacement
   held-out corpus after every generic change.
5. Start P4 only on a passing P3 subset and only with a real retained
   materializer. Compare baked-native and retained paths on identical traces,
   measuring wall time, p95, allocations/bridge/draw counters, and resource
   ceilings. A lower work counter alone is insufficient.
6. Promote only after behavior equivalence and a meaningful wall-clock/resource
   win; otherwise retain baked-native as authoritative and keep the experiment
   isolated.

## Fresh-session goal prompt

> Continue the importer reset plan from
> `planning/importer-reset-20261019-handoff.md` and the authoritative phase
> table in `docs/tools/importer-differential-lab.md`. Work in a fresh Pulp
> worktree. Complete P3a instrument-integrity, P3b real Chromium/native
> disagreement-floor calibration, and P3c replacement held-out freezing before
> changing importer rules. Preserve Skia/Dawn, use Chromium as the only browser
> oracle, do not introduce WebKit or a second production UI architecture, and
> do not lower the `0.995` threshold without an explicit evidence-based policy
> decision. Keep full-frame and registered metrics separate, report worst
> fixtures, update this plan as evidence arrives, and stop with an honest
> blocked/deferred result if the candidate consults Chromium or the measured
> renderer floor is below threshold. Only after P3a-c pass may the session
> pursue intrinsic sizing, typography placement, keyed-list correctness, and
> eventually a native-to-native baked-versus-retained performance comparison.

