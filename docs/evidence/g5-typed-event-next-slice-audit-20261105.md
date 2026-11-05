# G-5 typed-event next-slice audit

Date: 2026-11-05  
Source head: `91cd83b67bb863d268bb7b6ef52ab8573331c5a6` (`origin/main`)  
Worktree: `codex/g5-typed-events-20261104`

This audit checks the admitted bounded one-producer/one-consumer trigger lane
before widening the graph event surface. The current source already contains
the bounded runtime and its public consumer. No additional runtime API is
introduced here: the source audit found that arbitrary `CustomNodeType`
event-aware callbacks are deliberately not trigger producers or consumers.
Wiring them would require a new admission contract, so this lane remains
closed at the framework producer and framework envelope pair.

## Positive controls

The focused Release target was built with the governed Pulp CLI:

```text
pulp build --target pulp-test-group-trigger-graph
```

The executable completed with:

```text
All tests passed (566 assertions in 25 test cases)
```

Those tests cover the admitted same-rate feed-forward pair, sample and tie
ordering across block partitions, the 16-attempt/8-accepted per-sample
limits, the 4096-frame and 2 MiB resource bounds, reset and epoch exhaustion,
prepared replacement and live-snapshot retirement, mutation-free refusal
preflights, and zero allocations in a maximum-frame render.

The in-tree public consumer was also built and passed:

```text
ctest --test-dir build -R 'trigger-runtime-public-consumer' --output-on-failure
100% tests passed, 0 tests failed out of 1
```

An installed-SDK consumer receipt is preserved at
[`g5-trigger-runtime-installed-consumer.md`](g5-trigger-runtime-installed-consumer.md).
It builds outside the checkout against `Pulp::host`, executes the typed onset,
envelope, modulation, and low-pass path, and includes both an output mutation
and a private-header refusal control.

## Refusal and mutation controls

The focused suite asserts that the planner refuses non-canonical routing,
parallel or anticipated execution, multiple producers/consumers, non-zero
ports, non-trigger payloads, feedback/dependency cycles, hosted or MIDI nodes,
opaque Custom callbacks, invalid buffers, out-of-range and late emissions, and
attempts after the 16-attempt budget. Each refusal is checked before
publication and the existing live graph remains renderable; failed candidate
preparation and installation retain their candidate ownership.

The event callback surface in `custom_node_events.hpp` remains MIDI-only for
ordinary Custom nodes. `TriggerContext` is borrowed only by the framework
trigger generation and is not exposed as a general producer/consumer
registration hook. This is the deliberate refusal control for the next,
graph-authored event slice; it prevents an unbounded opaque callback from
silently bypassing the generation and resource ledger.

## Boundary and next action

The lane is **PARTIAL**. Host MIDI/MPE or absolute timestamps, fan-out,
feedback, external time projection, G-6 voice delivery, and plugin parity stay
outside this slice. A future graph-authored producer/consumer slice must first
define its public registration and edge-admission contract, then add a fresh
installed-SDK consumer and mutation/refusal receipts against that contract.
