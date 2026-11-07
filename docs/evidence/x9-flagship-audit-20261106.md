# X9 flagship acceptance audit (2026-11-06)

## Scope and source boundary

This audit starts from the clean `origin/main` source head
`3c11cb21c026799659e9cfe52d70f2121d46b41d`, then merges the three commits
that arrived on `origin/main` during the audit (final worktree head
`5491669d12d5d957b986ec26c91a47c9e514511d`). It exercises the checked-in
`pulp-test-signal-graph-x9-modular-instrument` fixture through public
`SignalGraph`, `ProcessorNodeInstance`, MIDI, state, serializer, and offline
host APIs. The fixture includes `examples/PulpSynth/pulp_synth.hpp`; this is an
in-tree example consumer, not an installed SDK consumer. No runtime API was
added by this slice, so an installed consumer is **N/A**.

The X9 acceptance rows require one application to combine polyphony, per-note
MIDI/MPE, modulation, reusable components, effects, presets, runtime mutation,
and offline replay. The rows below separate what the fixture measures from what
its dependencies still prevent it from claiming.

## Acceptance matrix

| Acceptance row | Current evidence | Disposition |
| --- | --- | --- |
| Bounded polyphony | The PulpSynth Processor owns a fixed eight-voice bank. The new case compares one MIDI note with a three-note chord and requires finite, different output. | **PARTIAL** — output differential proves more than one note reaches the Processor; no allocator telemetry, steal-policy receipt, note-off lifecycle, or graph voice binding is exercised. |
| Modulation | A graph automation lane drives `kFilterCutoff`; the new case compares low and high control input and requires different finite output. | **PARTIAL** — control-rate parameter response is proven; per-note MPE modulation remains outside the graph ingress contract. |
| Reusable components | The X3 case invokes one parameterized gain helper twice, verifies independent node identities and state, serializes/restores both, and checks independent mutation. | **PASS (bounded)** — this proves expansion into ordinary graph nodes, not a general nested-template runtime. |
| Effects / latency | Runtime mutation inserts one gain stage per output channel and changes the rendered result. | **PARTIAL** — effects are gain-only and zero-latency; no deliberate latency contribution or aux/sidechain/master path is proven. |
| MIDI | MIDI input node accepts note-on and chord events; the new case rejects publication to the audio output node as a planted negative. | **PARTIAL** — graph-relative MIDI ordering is exercised; host absolute timestamps, MPE sidecar delivery, fan-out, and overflow semantics remain G-5 work. |
| Presets and state | `plugin_state_io` serializes and restores the Processor StateStore cutoff value. Graph JSON emits a `processors` envelope. | **PARTIAL** — Processor state round-trip passes; graph restore of a runtime Processor remains an explicit refusal, so a complete preset-plus-graph restore is not claimed. |
| Runtime mutation | A prepared edit inserts two effect nodes and commits atomically. A malformed connection is rejected and the previous live snapshot handle remains unchanged. | **PASS (bounded)** — valid/invalid prepared-edit controls pass; continuity across latency-changing edits is not covered. |
| Offline rendering | The live and `OfflineSignalGraphHost` paths render the same event/input and compare every sample exactly. The integrated case renders two offline blocks. | **PASS (bounded)** — same-graph parity passes; no saved event-log replay or state/output hash join is emitted. |
| Hosted CLAP/VST3/AU | Conditional tests exist, but no format artifacts are present in this build. | **N/A** — availability receipt is false for all three formats; no hosted lifecycle claim follows. |
| Installed/public consumer | No public header or runtime contract changed; the fixture uses the in-tree PulpSynth example. | **N/A** for this test-only evidence slice. |

## G-program dependencies

* **G-1:** no `docs/graph-program-status/G-1.md` exists in this Pulp source
  head, so this audit does not infer a G-1 status. The row is **N/A / external
  authority required**.
* **G-4:** status is bounded first-slice composition and identity. Its explicit
  boundary excludes plugin, event, polyphony, and nested-subgraph declarations;
  therefore X9 cannot promote its voice-bank or preset claims from G-4 alone.
* **G-5:** status is **PARTIAL**. The bounded typed trigger runtime has consumer
  evidence, but host MIDI/MPE/time-index ingress is not routed into graph
  Processor nodes. X9's MIDI result is limited to the existing block-local MIDI
  lane.
* **G-6:** status is **IN PROGRESS / HOLD**. The allocator contract and installed
  consumer are evidence scaffolds; graph Processor/MPE delivery, event
  scheduling, persistence, and the reviewed G-4 voice binding remain
  prerequisites. This audit does not add a duplicate voice runtime.

## Focused receipt

The bounded fixture change is in
`test/test_signal_graph_x9_modular_instrument.cpp`. A governed Release build
completed with `pulp build --target pulp-test-signal-graph-x9-modular-instrument`.
The focused CTest run passed all five generated X9 cases:

```text
100% tests passed, 0 tests failed out of 5
Total Test time (real) =   0.30 sec
```

Receipt hashes from this worktree:

| Artifact | SHA-256 |
| --- | --- |
| source fixture | `80d7ca99b6d24ac0f5c7a5268c090517576cedff304ee46f5a2e4c1cd7af9ca5` |
| Release executable | `93e5737de840e31d8b97f3bc379fc4b4563cd9e1365cbab5ae8e99c5b388ad2a` |
| focused CTest transcript | `7ef434054c1f215d590ba778f8060c3f7beb83c5802951c043389ac4c826c65e` |
| direct executable transcript | `3604a05954901b2f8666c9dd5ec46c73debb21938b8fdbfcaad769e31c7e7842` |

The direct executable reported **1,828 assertions in 5 test cases**. This is
local Release evidence only; it does not establish cross-platform, hosted
format, hard-realtime, or installed-SDK parity.

## Safe next actions

1. Admit the reviewed G-5 host ingress and G-4 Processor voice binding before
   adding per-note MPE or graph-authored polyphony to X9.
2. Add a latency-bearing public effect and an aux/sidechain route only after the
   corresponding PDC and continuity contract is available.
3. Extend the fixture with a saved event log and state/output identity receipt
   once graph Processor restore is admitted; keep the current refusal control
   planted until then.

