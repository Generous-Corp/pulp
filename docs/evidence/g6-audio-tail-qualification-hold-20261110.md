# G-6 voice lifecycle and audio-tail qualification HOLD (2026-11-10)

## Exact base and disposition

This bounded audit starts from `origin/main` at
`d8cae12b3ae3e95941dc84fd6c52fc8b621921ed` in a fresh classified worktree.
Disposition: **HOLD; no runtime/API change and no duplicate G-5 work**.

The existing G-6 allocator consumer is a useful control probe, but it is
state-only. It does not establish graph voice expansion, Processor-owned voice
audio, or a graph tail oracle. The current public graph seams are sufficient to
describe the missing boundary, but not to close it safely.

## Verified current seams

- `core/host/include/pulp/host/voice_template.hpp:67-110` expands exactly
eight value-owned bindings and transactionally persists finite per-slot
parameters. Its own contract excludes active notes, generations, pedals,
epochs, queues, and release tails from persistence.
- `core/midi/include/pulp/midi/mpe_synth_voice.hpp:158-279` provides one
preallocated `MpeVoiceAllocator` with four steal policies, generation-aware
dispatch, releasing slots, and voice-owned `finish_release()`. It stores
concrete `Voice` objects; it has no graph slot binding, sample-interval
scheduler, sustain policy, or graph output ownership.
- `core/format/include/pulp/format/graph_runtime_executor.hpp:105-136` now
exposes routed audio views, MIDI, and a borrowed `node_mpe_in` buffer to a
Processor binding. `core/format/src/processor_node_adapter.cpp:253-269`
installs that borrowed sidecar only for the duration of `Processor::process()`.
This is an ingress seam, not a voice allocator or per-voice graph expansion.
- `core/format/include/pulp/format/processor.hpp:144-149` requires the
Processor callback to be allocation-free, lock-free, exception-free, and
free of I/O. No current G-6 fixture measures the required WCET, allocation,
lock, or I/O matrix for a graph voice bank.
- `examples/g6-voice-contract-consumer/main.cpp:9-18,47-86,119-165` proves
allocator state transitions, generation rejection, release-slot reuse, four
steal policies, and bounded event overflow. Its `Voice::render()` only
advances smoothers; it produces no audio and no tail oracle.

## Missing gates (not inferred from existing evidence)

The following remain unproven and are required before G-6 can advance:

1. A reviewed G-4 Processor voice binding mapping stable authored identities to
prepared per-voice state without control-thread graph mutation in the audio
callback.
2. A fixed-capacity graph voice consumer using the existing allocator, with
sample-accurate event intervals, cross-block offsets, sustain/pedal ordering,
stale-generation rejection, overlapping same-pitch notes, and all four steal
policies.
3. Positive audio output from each voice plus an independent tail oracle that
distinguishes finite release completion from silence, with planted routing,
timing, state, and cross-voice-isolation negatives.
4. Atomic save/restore refusal for active generations, unsupported tail
checkpoints, stale identity, and partial mutation; the current G-4 state
schema intentionally does not represent these values.
5. Realtime evidence for the graph voice path: callback WCET, allocation/lock/I/O
checks, sample-rate and block-size matrix, long-run event loss, and hosted
adapter boundaries.

## Why no implementation is authorized in this slice

Adding an allocator wrapper now would duplicate the existing MPE allocator and
would invent a second graph/voice binding contract. Adding a graph voice API
would also overstate the current G-4 schema, which is explicitly control-side,
and would make the existing state-only consumer look like an audio proof. The
safe successor is to admit the reviewed G-4 Processor binding and then build one
fixed-capacity public consumer around the existing allocator. Until that seam
exists, this HOLD is stronger evidence than a speculative fixture.

## Required successor evidence

The successor must use published headers and a governed Release build. Its
receipt must bind the exact source, SDK, configuration, executable, and fixture
hashes, and include positive and independently planted-negative controls. It
must state any unavailable hosted, Linux, or Windows lanes as `NOT CHECKED` or
`HOLD`; no graph voice or hard-realtime claim may be inferred from this audit.
