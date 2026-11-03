# G-11 modulation cookbook evidence

Date: 2026-11-03  
Fixture source: `origin/main` at `a52cf1b5bf9141a93de6efcbc839dfda344df8dd`  
Platform scope: source-only API audit on macOS arm64

## Bounded verdict

**PARTIAL / cookbook documentation lane.** The modulation-toolkit cookbook is
useful as a recipe index, but two snippets had drifted from the public API:
the random-step clock omitted the required period expression, and the ratchet
recipe passed a floating-point signal to `TriggerDetectT::process()`, which is
intentionally deleted. Both snippets now use the published APIs. This slice
does not claim that every prose recipe is a complete plugin.

## Changes

- P4 now uses the same `units::division_to_samples()` expression as P3 to
  calculate a sixteenth-note period.
- P5 now calls `TriggerDetectT::process_signal()` for the floating-point
  trigger input. `process(bool)` remains the decoded-event API.

## Controls

The representative P1 and corrected P5 compositions were checked with a
C++20 syntax-only probe against the public signal headers. The probe used the
same calls shown in the cookbook, including `Vca::Response`, `BurstGenT::Hit`,
`Lpg::velocity_to_strike()`, and `TriggerDetectT::process_signal()`.

The planted negative was the pre-fix P5 call to `hit.process(trigger)`: the
compiler rejected it because the floating-point overload is explicitly deleted.
This confirms the documentation change is tied to a real API boundary rather
than a stylistic rewrite.

## Scope and next action

No runtime or build-system code changed. A full cookbook consumer would be a
separate G-11 slice; the current recipes intentionally mix complete snippets
with prose compositions and application-owned signal variables. The next safe
step is to add a compile-backed consumer only if G-11 requires executable
coverage for all nine recipes.
