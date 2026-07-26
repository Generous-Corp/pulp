# Modulation toolkit post-merge missing items

## Succinct goal

Make `LpgT::cutoff_hz()` truthful across `reset()`, prove the reset-state
coefficient and telemetry stay synchronized, and land the fix without
reopening the already-closed G1-G5 work.

## Audit basis

The final audit ran on freshly fetched `origin/main` at
`2ff8ada9c11e751967168db71554ee1b905c4037`, after modulation-toolkit gap
closure PR #6590 merged as
`04f96ef13554aa5b1cd692592f64192a5b3a3057`.

The five focused merged-tree suites passed unchanged:

- `pulp-test-signal-mod`: 59 cases, 1,362,066 assertions
- `pulp-test-signal-mod-events`: 36 cases, 115,743 assertions
- `pulp-test-signal-mod-voice`: 24 cases, 236,471 assertions
- `pulp-test-signal-mod-rt-safety`: 7 cases, 7 assertions
- `pulp-test-forge-modulation-catalog`: 18 cases, 76,482 assertions

That is 144 cases and 1,790,769 assertions. G1-G5 remain closed.

## Confirmed missing item

### M1 — LPG cutoff telemetry is inconsistent across reset

PR #6590 introduced `LpgT::cutoff_hz()` as the effective cutoff commanded by
the most recent `process()` call. Its `reset()` implementation then overwrote
the telemetry with `fc_min_`, while `TptFilterT::reset()` cleared only the
integrator state and retained its existing cutoff coefficient.

After a high-control process followed by `reset()`, the accessor therefore
reported a cutoff that had not been commanded to the filter. On a reset cell
with the default colour, the next process also derives a logarithmic midpoint
cutoff rather than `fc_min_`.

Close this by using one cutoff-command helper from both `reset()` and
`process()`, then add a regression test proving the reset-state telemetry
matches the commanded logarithmic cutoff and remains stable on the next
zero-control process. Because `reset()` now commands the filter during
`prepare()`, the LPG must also keep the filter's 1 Hz lower cutoff bound valid
when callers supply a non-positive sample rate.

## Acceptance

- `reset()` clears the vactrol and filter state, commands the cutoff derived
  from that reset state, and reports the same value through `cutoff_hz()`.
- `prepare(0)` uses a safe fallback rate, reports a finite 1 Hz cutoff, and can
  process without invalid filter clamp bounds.
- The regression test fails on merge commit `04f96ef1` and passes with the fix.
- All five focused suites remain green.
- The follow-up lands through normal required gates with no change to branch
  protection or advisory-lane policy.

## Follow-up validation

The clean Release build passed all five focused suites with the two new
regressions included: 146 cases and 1,790,774 assertions.
