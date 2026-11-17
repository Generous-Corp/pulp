# G-5, G-6 and P-1 review findings left open (2026-11-15)

Two independent reviews (adversarial, and code quality) read `origin/main` at
`6b072721d25ae1ed1af54051770be66e5e293b54` for the ControlIndex, host-event
timestamp, HostBlockTimeline, VoiceBankBinding, parameter-mailbox and P-1
consumer slice. The must-fix and the safe should-fix findings were fixed with
tests on branch `graph/ffix-control-events-20261115`. This file records the
findings that were not fixed there, with the reason and the change each one
needs.

## Needs an owner decision

### One scalar Control library instead of two

Resolved by consolidation without removal: `control_math.hpp` is canonical
and the redundant `indexed_control.hpp` spellings are deprecated forwarding
wrappers. Their removal waits for an announced breaking SDK release; see
`D3-scalar-control-consolidation.md`.

### Result vocabularies

Six overlapping result enums exist (`HostEventResult`, `GraphEventResult`,
`GraphEventBindingResult`, `TriggerEmitResult`, `TriggerGraphResult`,
`ControlProcessResult`, plus `VoiceBankBindingCode`). Exhaustion is spelled
`ClockExhausted`, `Overflow` or `EpochExhausted`. Only `ControlProcessResult`
has name and fix strings.

The concrete defect (VoiceBankBinding returning Busy and OutOfRange where the
ingress returns StaleEpoch and Busy) is fixed. Adding `*_result_name()` for
`HostEventResult`, `GraphEventResult` and `VoiceBankBindingCode`, or unifying
the enums, is a public API design change.

## Too large for a minimal fix

### AU event-time conversion implemented twice

`au_event_block_offset()` (`core/format/include/pulp/format/detail/host_sample_position.hpp`,
clamps) and `au_event_block_time()` (`core/host/include/pulp/host/host_event_timestamps.hpp`,
refuses) copy the immediate-window test, the 4096-frame window constant and the
overflow-checked subtraction. The change: one core in `host_sample_position.hpp`
returning `std::optional<std::int64_t>` for the raw offset, with the adapter
clamping and the projection refusing, and one window constant. This touches
the AU adapter's shipped event path and needs AU validation, so it is its own
change.

### One 4096-frame bound

The block bound is an unlinked literal in `host_event_timestamps.hpp`,
`graph_event_contract.hpp` (five places), `control_index_nodes.hpp`,
`voice_bank_binding.cpp` and `host_sample_position.hpp`. The change: one
`kGraphEventMaxFrames` in `graph_event_contract.hpp` that the others alias, or
`static_assert` equality where a header cannot include it.

### P-1 consumer is one 1800-line translation unit

`examples/p1-event-admission-consumer/main.cpp` holds six independent receipts
(RTSan boundary, host/parameter, graph lane, parameter mailbox, voice bank,
combined/parity). Splitting it per family is a mechanical but large move; the
misleading epoch-map comment and status sentence were corrected instead.

### Planning-coded names in shipped examples and tests

Catch2 tags in the slice now say what they cover. The test case names in
`test/test_voice_bank_binding.cpp` (23 cases prefixed `G6`), the
`test/test_g5_*.cpp` and `test/cmake/g5_*.cmake` files, and the 19 example
directories named `examples/g<N>-*` or `examples/p1-*` still carry program
codes. Case names are ctest names that evidence documents and the paced-matrix
tooling reference, so renaming them is deferred to a change that updates those
references together. Tags carrying other program codes (`[g1]`, `[g3]`, `[g4]`,
`[g7]`, `[g12]`) are outside this slice.

## Resolved since

- MIDI input mailbox drops an unread batch with no telemetry: resolved with
  the recommended option. `SignalGraph::MidiMailboxCounters` and
  `midi_mailbox_counters(NodeId)` mirror the parameter mailbox counters, and
  `inject_midi_into_snapshot_()` publishes through `write_replacing_unread()`.
  `inject_midi()` keeps its return value. Covered by the test case
  `SignalGraph MIDI mailbox counts published, overwritten and dropped events`.

## Nits not taken

- `HostEventIngress::begin_block()` accepts a window that moves backwards
  within one epoch. Refusing `sample_start` below the previous window end
  would also refuse the documented retry of a refused block at the same
  start, so the rule must use the consumed cursor, which the ingress does not
  track today.
- A graph with only a Control index input allocates a full
  `GraphAuthoredEventLane<>` (about 2 MB of record arenas) and clears two
  8192-entry budget arrays per block. Not a correctness issue.
- The planted negatives for `parameter-mailbox-overflow-counters` and the
  cross-run voice-epoch clause are not reproducible from the tree; nothing
  records the mutation that was planted.
- `kControl*TypeId` constants for the built-in Control node ids,
  `validate_control_node_state()` ignoring the type version,
  `TripleBuffer::write()` delegating to `write_replacing_unread()`,
  `ParameterMailboxCounters` field names versus its atomics, `clock_from_au`
  not being constexpr, and the `InputSemantic` cast without a `static_assert`.
