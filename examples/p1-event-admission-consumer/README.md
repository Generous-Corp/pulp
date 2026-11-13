# P1 event admission public consumer

This installed-SDK consumer is the bounded admission fixture for P1. Each of
the four typed families runs through a public seam across at least two blocks
and two partition schedules:

* absolute MIDI host ingress, stable producer ordering, and stale-epoch, late,
  and duplicate-sequence refusal (partitions 4 and 8);
* triggers through the graph-authored event lane: a `CustomNodeType` producer
  and consumer joined by `SignalGraph::connect_graph_event`, emitting with
  `GraphEventWriter::emit_at` and reading `GraphEventReader::events`. It covers
  current-block and bounded-future delivery across a block boundary, `Future`,
  `Late`, and `Overflow` refusals, reset cancelling a staged record with an
  epoch advance, and identical output for the 2+2+4 and 3+5 schedules. Public
  `TriggerContext` is not a consumer seam and is not used; the existing trigger
  graph check is kept as trigger-graph evidence;
* parameters through `SignalGraph::inject_parameter_events` into a small
  `PluginSlot` node added with `add_plugin_node`: latest-wins, sorted delivery
  with stable equal-offset order, one-shot delivery, and a source-side
  overflow that returns `false` (block sizes 4 and 8). A standalone
  `ParameterEventQueue` sort and overflow check is also kept;
* voice intent through the installed `pulp::host::VoiceBankBinding`: note-on
  and note-off land across blocks on one slot and generation, and
  `dispatch_deferred` refuses wrong, retired, and re-used-slot generations with
  `StaleGeneration` while a live generation is accepted (partitions 4 and 8).
  This is control-side evidence, not a graph voice allocator.

A combined control then drives all four families through one test-owned epoch
and absolute sample origin on the same block boundaries: a control-side adapter
projects `HostEventIngress` MIDI and injects block-relative events with
`SignalGraph::inject_midi` into a MIDI input node, converts absolute parameter
records to block-relative `inject_parameter_events` into a `PluginSlot` node,
a graph-authored lane producer in the same graph emits at absolute targets, and
a `VoiceBankBinding` renders on the same blocks. The canonical (family,
absolute sample, order key) trace must be identical across five schedules
(1, 32, 64, 127+127+2, 3+61+1+127+64). In-binary planted negatives change one
record's order key per family, and swap two equal-sample MIDI records; only
that family's digest may change, and a record diff must name only the touched
records. `--trace <path>` writes the canonical trace and digests as JSON lines.
Callback-side observation uses preallocated fixed arrays.

It adds no runtime queue, voice-intent API, or graph seam. The fixture is qualified only when built from a complete installed
SDK and the positive executable exits zero; the receipt stays `NOT_QUALIFIED`
until the remaining admission rows are checked. Missing SDK support modules are
`NOT_CHECKED`, not a qualification claim.

```sh
cmake -S examples/p1-event-admission-consumer -B build-p1-event-admission \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK_PREFIX"
CMAKE_BUILD_PARALLEL_LEVEL=1 tools/ci/governed-build.sh cmake --build \
  build-p1-event-admission --target pulp-p1-event-admission-consumer
ctest --test-dir build-p1-event-admission --output-on-failure
```
