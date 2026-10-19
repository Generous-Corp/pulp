# Importer reset handoff (2026-10-19)

Status: **PAUSED before additional implementation.** The goal remains
unfulfilled. P0, P1, and P2 are complete; P3 is open; P4 and P5 have not
started. This document is the restart packet for a fresh session.

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

## Adversarial review findings

Fable reviewed the plan and current receipts. Before any fixture-specific or
generic importer tuning:

1. **P3a: instrument integrity.** Prove candidate-stage provenance shows no
   Chromium consultation. Reconcile the unexpectedly similar roughly 9.7-second
   native timing. Pin comparator version/digest in every receipt and explicitly
   re-baseline after comparator changes.
2. **P3b: renderer disagreement floor.** Run authenticated same-source
   Chromium/native known-good controls three times, with a planted negative
   control, and report the floor by fixture class beside the `0.995` threshold.
   Do not lower the threshold during this session.
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

