# A1 graph audio and tail oracle gap (2026-11-10)

## Disposition

**HOLD: evidence only; no graph runtime or public API change.** This audit is
bound to Pulp `origin/main` at `ba04406f050258968f64d497fd6e80937618da68`.
It asks whether the existing public surfaces can prove the A1 fixed-voice
graph requirement. They cannot. The current tree has useful control-side
identity, event, and audio examples, but no graph-authored voice instance that
renders audio and owns a finite tail.

This packet does not change the roadmap or turn an instrument example into a
graph qualification. It records the smallest safe successor and the controls
that must be retained when that successor is authorized.

## Verified current surfaces

The following findings come from the exact base above, rather than from older
planning claims:

| Surface | What is verified | What it does not prove |
| --- | --- | --- |
| `docs/graph-program-status/G-4.md` and `pulp/host/voice_template.hpp` | An authored template can expand eight deterministic slot records, derive value-only identities, and transactionally restore declared control state. | No prepared graph voice instances, audio ownership, note generations, event scheduling, or tail lifecycle. |
| `core/format/include/pulp/format/graph_runtime_executor.hpp:105-132` | A prepared graph node context now carries MIDI, UMP, and an MPE sidecar pointer. | No mapping from a G-4 slot identity to a Processor voice, and no voice allocator or per-voice output mix. |
| `core/format/src/processor_node_adapter.cpp:107-275` | Processor bindings save and restore the node's event sidecars around dispatch. | No A1 graph voice binding, sustain policy, stale-generation admission, or audio/tail oracle. |
| `core/midi/include/pulp/midi/mpe_synth_voice.hpp` | The existing allocator is preallocated and generation-aware, with documented steal policies and release bookkeeping. | The allocator is not bound to a prepared SignalGraph plan and its installed consumer is state-only. |
| `examples/design-panel-instrument/` | A non-graph `Processor` example contains an eight-voice synth and explicit audio assertions. | It exercises a direct Processor path, not graph authoring, graph dispatch, graph swaps, or graph-owned voice state. |

The G-5 sidecar wiring is therefore a real dependency improvement, but it does
not close A1 by itself. Treating the sidecar pointer as a voice binding would
conflate event delivery with graph polyphony.

## Existing audio controls and their boundary

`examples/design-panel-instrument/test_design_panel_instrument.cpp` is the
closest existing audio oracle. Its controls are concrete and should be reused
by an eventual graph fixture:

| Control | Current assertion | A1 relevance | Current-head status |
| --- | --- | --- | --- |
| Idle silence | Every sample is exactly `0.0`; active voice count is `0`. | Detects stale output and uninitialised graph state. | Source contract present; no exact-current graph run. |
| Note output | `peak(output) > 0.01`; one active voice. | Positive audio path and voice admission. | Direct Processor only. |
| Release tail | After note-off, active count is `0` and final quarter RMS `< 0.001`. | Finite tail and reclamation. | Direct Processor only. |
| Polyphonic chord | Three simultaneous notes leave three active voices. | Detects monophonic collapse. | Direct Processor only. |
| Retrigger | Same note at offsets `0` and `64` leaves one active voice. | Prevents duplicate-note stacking. | Direct Processor only. |
| Extremes | Eight-note chord remains finite and `abs(sample) < 16`. | Bounded output under full voice load. | Direct Processor only. |
| Mutated routing/threshold | The test source has independent controls for filter brightness and parameter binding. | Provides planted-negative shape for a future graph oracle. | No graph mutation receipt. |

These are **existing source-level acceptance controls**, not a new A1 receipt.
The current tree has no exact-head receipt showing these controls through a
SignalGraph voice plan.

## Current-head execution attempt

I attempted a governed Release build of the existing audio fixture with:

```text
build/pulp build --examples --target pulp-design-synth-test
```

The checkout's CLI was rebuilt first to v1.98.0. The governed configure then
failed before compiling the target because `AgentToolkitSynth_Core` and
`AgentToolkitSynth_Standalone` were each registered twice while CMake was
regenerating the shared build tree. The failure is a build/configuration
harness issue, not evidence that the audio fixture fails. No test executable
from this exact base was run, and no current-head audio measurement is claimed.

The failed configure is deliberately retained as a blocker: running a stale
binary from another checkout would not establish exact-head provenance.

## Missing A1 gates

The following gates remain unproven and are required before A1 can be marked
complete:

1. A reviewed G-4 Processor voice binding that maps each stable slot identity to
   one prepared graph voice state without callback graph mutation.
2. A fixed eight-voice graph consumer using the existing allocator semantics,
   with positive audio output through the graph executor.
3. Sample-offset and cross-block partition invariance for note-on, note-off,
   expression, sustain/pedal, and overlapping same-pitch notes.
4. All documented steal policies, stale-generation expression refusal, per-
   voice state isolation, finite release tails, and graph output reclamation.
5. An independent audio oracle with positive and planted-negative routing,
   timing, state, and output controls. The oracle must compare rendered audio,
   not only active-voice counters.
6. Realtime qualification: no callback allocation or lock, bounded work,
   reset/reprepare behavior, and a measured deadline matrix.
7. A clean installed-SDK/public-consumer run proving the graph voice path from
   outside the source tree, including exact SDK and source provenance.

## Smallest authorized successor

After the reviewed G-4 Processor voice binding is admitted, implement one
bounded fixture rather than a general polyphony API. It should instantiate
eight fixed slots in one prepared graph, feed a deterministic note trace, and
render a simple oscillator/envelope path whose scalar reference is independent
of the graph implementation. Run the controls above plus block-partition,
steal-policy, stale-generation, sustain, tail, and mutation cases. Keep graph
authoring and allocation on the control/prepare side; the callback should only
consume preallocated state. Publish the resulting receipt from a fresh exact-
main worktree and keep unsupported host/platform lanes explicitly HOLD.

Until that successor exists, A1 remains **HOLD**. The existing direct
instrument test is valuable oracle material, but it cannot be promoted to graph
polyphony evidence by documentation alone.
