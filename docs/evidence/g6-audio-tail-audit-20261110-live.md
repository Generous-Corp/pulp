# G-6 live voice/audio-tail qualification audit — 2026-11-10

Status: **HOLD / evidence-only**. This audit starts at exact `origin/main`
`d33f5ea7d8e9f7ef2fd54cd3f90fbe44e0e6eb43` and records what the current public
surfaces prove. It does not add a graph runtime or promote direct voice tests
to graph evidence.

## Verified existing controls

| Surface | Verified capability | Boundary |
| --- | --- | --- |
| `pulp::midi::MpeVoiceAllocator` and `examples/g6-voice-contract-consumer/main.cpp` | bounded eight-voice allocation, generation-safe expression, stealing, release-slot retention, explicit tail completion, reuse reset, and overflow rejection | state-only consumer; `Voice::render` only advances smoothers and never produces graph audio |
| `pulp::audio::VoiceRuntimeFacade` | borrows an existing MIDI synthesiser or instrument allocator and exposes note/controller dispatch, active/releasing counts, and telemetry | facade is non-owning and is not a graph `Processor` binding or graph-owned voice state |
| `test/test_drum_kick.cpp` | direct voice render, inactive silence, release completion, and choke-tail behavior | direct DSP unit tests; no graph manifest, graph event admission, or installed graph consumer |
| `docs/evidence/a1-audio-oracle-gap-20261110.md` | confirms the existing direct Processor instrument controls and their positive/negative boundaries | no graph voice audio/tail receipt; the prior configure attempt was stopped on duplicate CMake targets |

These are useful prerequisites, but they are independent surfaces. A direct
voice or Processor result cannot prove that a prepared graph routes note events
into a voice, renders audio, or retains a release tail across graph blocks.

## Missing gates that keep G-6 on HOLD

1. A reviewed G-4 manifest-to-Processor binding that maps stable slot identity
   to an actual graph-owned voice instance.
2. Graph event admission for note-on, note-off, sustain and expression across
   block boundaries, including stale-generation and reuse refusal controls.
3. A fixed-capacity graph consumer whose `SignalGraph::process` output is
   non-silent for a positive note case and differs for an independent planted
   negative control.
4. A graph-owned release-tail receipt proving nonzero post-note output followed
   by bounded quiet, with explicit tail completion and slot reuse.
5. Exact-current installed-consumer, callback WCET/allocation/lock/I/O, and
   non-Darwin/hosted boundaries for the same graph voice path.

The current public code has no authorized seam for these claims. Adding a
placeholder graph Processor binding or declaring the direct tests equivalent
would overstate the evidence and violate the roadmap's evidence boundary.

## Safe next experiment

After G-4 Processor binding is reviewed, add one fixed eight-voice graph
consumer using only that public seam. Require a positive audio/tail oracle,
stale-generation and restore refusals, and an independent planted negative
before attempting realtime or cross-platform qualification. Until that seam
exists, this document is a durable HOLD rather than an implementation claim.

Source and receipt state: no runtime/API changes, no generated receipt, and no
graph audio claim. Heavy dedicated-builder suites are **NOT CHECKED** for this
docs-only audit.
