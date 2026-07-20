# Sampler suite architecture

**Status:** accepted current architecture

**Scope:** CPU audio, storage, import, analysis, and verification paths

## Playback systems

Pulp intentionally has three playback ownership models:

1. `StreamingSampleSource` is a sequential preload-plus-SPSC-tail transport
   for one ordered consumer.
2. `SampleAsset` with `SampleStreamService` is a shared page-cache system for
   polyphony, random access, reverse traversal, loops, crossfades, and prepared
   interpolation across page boundaries.
3. `PublishedSampleStore` and `PublishedSampleView` provide resident,
   generation-retained playback for decoded, recorded, or offline-rendered
   audio.

These systems are not interchangeable descriptors. `PublishedSampleView` is a
resident store-generation handle; `SampleAssetView` combines preload memory,
metadata, registration proof, and an optional stream identity. Their common
seam is behavioral:

```text
LoopRegion → LoopPlaybackCursor → PreparedSampleInterpolation → storage adapter
```

`LoopReader` owns canonical resident tap mapping. `LoopRenderer` orchestrates
resident traversal. `SampleStreamLoopVoiceReader` applies the same cursor and
interpolation contracts to paged storage. `SampleStreamVoiceReader` remains the
smaller forward one-shot adapter.

## Compatible instrument layers

`SampleVoiceRenderer` remains source-compatible and owns pool resolution,
optional envelope integration, steal fades, and accumulate/overwrite behavior.
Its supported region tap addressing delegates to `LoopReader`; new rich-loop
engines should use `LoopRenderer` rather than extend its public render-state
layout.

`InstrumentRuntime`, `InstrumentVoiceAllocator`, and `AhdsrEnvelope` are
composable policies, not mandatory sampler architecture. Products may retain
distinct voice allocation, sustain, retrigger, and automation behavior while
sharing the lower-level mapping, analysis, cursor, interpolation, and storage
contracts.

PulpTempoSampler demonstrates that boundary. It keeps fixed-first-voice
stealing, sustain and held-loop re-engagement, panic and play-through semantics,
and dynamically updated `signal::Adsr`. It shares `SampleKeyMap` and
`SlicePointAnalyzer`, performs tempo matching off the callback, then publishes
one fixed-capacity `TempoSamplerPublishedSource` containing the resident sample
view and its matching slice regions.

## Analysis and mapping

`SampleKeyMap::slice_index_for_note()` is the allocation-free note-to-slice
primitive used by full `SliceMap` resolution and product trigger paths.

`SlicePointAnalyzer` keeps the existing timeline-order and near-zero behavior
as its default overload. Additive selection options support:

- an optional maximum region count;
- timeline-order or deterministic strongest-confidence selection;
- near-zero or sign-transition snapping.

Strongest selection snaps candidates first, ranks confidence and source policy
deterministically, enforces minimum distance from the origin, endpoint, and
already-selected cuts, then returns the chosen boundaries in timeline order.

## Realtime ownership

The callback reads immutable or generation-retained data and prepared fixed
storage. It may plan cursor frames, evaluate interpolation, push bounded demand
or cancellation commands, bracket cache-page reads, render paged voices, and
advance starvation envelopes. It must not decode, mutate cache ownership,
prepare assets, run analysis, acquire memory leases, or publish resident data.

The sampler RT contract inventories representative core operations from
`SampleAssetView`, prepared interpolation, `LoopPlaybackCursor`, both paged
voice readers, the stream service, starvation envelopes, and memory governance.
Focused tests enforce that callback-allowed rows cannot allocate, lock, or
block. PulpTempoSampler separately runs prepared steady-state processing under
the realtime allocation/lock probe.

## Metrics and tools

`SamplerLooperMetricsSnapshot` remains the older recorder/capture/slot/analysis
summary. Shared streaming diagnostics remain in `SampleStreamServiceStats` and
PulpSampler's typed aggregation. Resident PulpTempoSampler playback does not
invent stream-service metrics.

The sampler CLI covers both asset operations and the separate Heritage profile
interchange/verification boundary:

```bash
pulp audio sampler-mip build source.wav --levels 2 --json
pulp audio validate summarize render.wav --json
pulp audio heritage validate profile.json --json
pulp audio heritage canonicalize profile.json --out canonical.json
pulp audio heritage inspect canonical.json --json
pulp audio heritage render canonical.json --fixture impulse \
  --out impulse.wav --report impulse.json
```

See the [sampler playback chooser](../guides/sampler-playback.md),
[PulpSampler](../examples/pulp-sampler.md), and
[PulpTempoSampler](../examples/pulp-tempo-sampler.md) for integration guidance.

## Deliberate boundaries

- CPU playback is the supported path; sampler correctness does not depend on a
  GPU.
- No universal resident/streamed source descriptor or virtual sample source is
  required.
- No public `SampleVoiceRenderState` layout change is required.
- `LoopInterpolationMode` remains a compatibility shorthand;
  `PreparedSampleInterpolation` is the canonical new-code surface.
- Generic allocator, AHDSR, or metrics expansion requires a concrete consumer
  whose policy matches.
- Sample Heritage composes after canonical source traversal and interpolation;
  it does not replace storage, loop, mapping, analysis, or starvation policy.
- Heritage live/commit cyclic resynthesis is intentional character processing,
  not a replacement for conventional `signal::OfflineStretch` tempo matching.
- Pulp ships the neutral profile SDK and authoring workflow, with no bundled
  named profiles. Optional captures or listening notes calibrate a particular
  profile; they are not SDK release gates.
