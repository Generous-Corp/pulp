# Adversarial review of graph waves 2-3: repairs (2026-11-18)

Scope: the confirmed correctness and test-gap findings from the second-model
review of the merged persistence, adapter-timing and host-timeline work. Each
finding was re-read against `origin/main`; the fixed ones were reproduced by a
test that failed before the fix.

## Fixed

| Finding | Repair | Test |
|---|---|---|
| A refused note-off (time at or past the end, negative, or in an unrendered oversized tail) left a note held, and `refused_events` is only a count | every adapter (CLAP, VST3, AU v3, LV2) requests `ProcessContext::reset_requested` on the next block the Processor observes and resets its MPE tracker, as CLAP already did for a dropped stateful event; the request survives a skipped block. Policy in `D4-adapter-timing.md` | `CLAP requests a reset after refusing a host event so no release is lost` (`pulp-test-process-timing`), `VST3 requests a reset ...` (`pulp-test-vst3-process-timing`), `LV2 run() requests a reset ...` (`pulp-test-lv2-adapter`), and the AU v3 MIDI placement case (`pulp-test-au-plugin-state`). The CLAP and VST3 cases drive a voice-tracking probe through the real adapter: before the fix the note stayed held after the next block |
| The document diff keyed event connections by endpoints and ports only, so `[kind 1, kind 2]` against `[kind 2]` compared equal and record order changed the result | records on one endpoint pair are paired by sorted attributes, like parallel audio connections, so neither overwrites the other | `the document diff keeps event connection records that differ only by kind` (`pulp-test-graph-event-persistence`) |
| The `Diverged` detail counted only node and connection differences, so an event-connection or spectral-asset edit read as "0 node and 0 connection differences" | the detail counts node, connection, event connection, spectral asset and section differences | `an event connection edited outside the history is named in the Diverged detail` (`pulp-test-graph-event-persistence`); the asset-only divergence in `Spectral asset history retains a bounded total of bytes and refuses past it` (`pulp-test-spectral-graph-saved-identity`) |

## Not reachable today

The trigger-graph branch that advances the graph cursor after a routed
processing failure (`SignalGraph::process_checked` in `trigger_graph.cpp`) is
not reached by any graph a trigger plan admits. It needs a routed node to fail
after the trigger producer has run. `prepare_trigger_plan` refuses Plugin and
Processor nodes, spectral graphs and sample regions with `Mode`, and every
other node it admits (audio and MIDI I/O, gain, the trigger pair and
trigger-audio Custom nodes) has a routed binding that cannot fail
(`custom_binding` and the gain binding always return true; I/O nodes have no
binding). Setup failures inside the serial loop need a port count above the
64-port limit, which the plan's budget check also refuses. Host-ingress
refusals, including the MPE preflight the binding test exercises, return before
routed execution. The branch is kept as a guard for a future admission of
Processor nodes, and its comment says so; there is no test for it, because no
admitted graph reaches it.

## Left open

- The isolated CLAP launch-deadline test evaluates `launch_exchange_deadline()`
  on synthetic time points; nothing starts a worker that delays hello and load.
  The production arithmetic was checked and is correct, so this is a coverage
  gap. A delayed-worker fixture (about 3 s hello plus 3 s load succeeds; a
  stalled worker times out and is reaped) would close it.
- `graph_serializer.cpp` is about 4,400 lines with event-connection validation,
  writing and rebinding in three separate regions; extracting them into an
  event-connection persistence file, as spectral persistence already is, is
  the next split. `trigger_graph.cpp`, `cmd_graph.cpp` and
  `signal_graph_runtime.hpp` are near the same size.
- Several test files mix concerns and exceed 1,000 lines
  (`test_spectral_graph_saved_identity.cpp`, `test_graph_history.cpp`,
  `test_host_timeline_binding.cpp`, `test_isolated_clap_worker.cpp`); split
  them by concern when next touched.
