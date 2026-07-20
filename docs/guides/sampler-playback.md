# Choosing a sampler playback surface

Pulp has two streaming systems and a resident publication path. They are
purpose-built rather than interchangeable: choose by access pattern and
ownership model.

| Need | Recommended surface |
|---|---|
| Sequential, ordered, single-consumer playback | `StreamingSampleSource` |
| Shared polyphonic/random/reverse/looped streaming | `SampleAsset` + shared service + loop voice reader |
| Resident generation-published playback | `PublishedSampleStore`/`PublishedSampleView` + `LoopRenderer` |
| Zone/pool instruments | `SampleZoneMap`/`SamplePool`/`InstrumentRuntime`, with allocator/envelope only when their policy fits |
| Offline tempo matching | Offline stretch, then resident publication as demonstrated by PulpTempoSampler |
| Data-defined sampler character, clock/converter behavior, or intentional cyclic resynthesis | Sample Heritage profile around the chosen resident or streamed voice path, as demonstrated by PulpSampler |
| Slice/key analysis | `OnsetDetector` → `SlicePointAnalyzer` → `SliceMap` → `SampleKeyMap` |

## Sequential playback

Use [`StreamingSampleSource`](streaming-sample-source.md) for one ordered
playhead. It owns a resident preload, one background producer, and one SPSC
tail. Its underrun policy preserves source order, which is useful for linear
playback but is not a shared random-access cache.

## Shared paged playback

Use [`SampleAsset` and the shared stream service](sample-stream-service.md)
when several voices can request the same pages or traverse out of order.
`SampleStreamVoiceReader` is the narrow forward one-shot adapter.
`SampleStreamLoopVoiceReader` adds cursor-driven reverse, loop, crossfade, and
prepared-interpolation planning. Choose the narrow reader when those richer
policies are unnecessary.

Build persisted streamed octave mips off the audio thread with:

```bash
pulp audio sampler-mip build source.wav --levels 2 --json
```

The existing `pulp audio validate` commands can inspect rendered WAVs. Sampler
asset playback APIs do not add commands, while the separate profile layer uses
`pulp audio heritage validate|canonicalize|inspect|render`.

## Resident playback

For decoded, recorded, or offline-rendered audio that fits the resident
budget, publish through `PublishedSampleStore` and retain generations until
active voices release them. The canonical resident playback seam is:

```text
LoopRegion → LoopPlaybackCursor → PreparedSampleInterpolation → LoopReader
```

`LoopRenderer` orchestrates that seam. `SampleVoiceRenderer` remains useful
when an instrument already owns pool resolution, optional `AhdsrEnvelope`,
steal fades, and accumulate/overwrite policy, but it is not the preferred
general rich-loop engine.

## Heritage character processing

Use the [PulpSampler Heritage Kit](../examples/pulp-sampler.md#sample-heritage-kit)
after choosing a storage and traversal model. It prepares one character engine
per voice and an optional post-mix bus. The existing reader still owns source
order, reverse, loops, crossfades, interpolation, mips, and starvation; Heritage
then applies its machine-domain clock, pitch family, converter, live cyclic
stretch, and hold, returns each voice through its fixed reconstruction/color
frame, and finally applies bus stages.

Heritage live cyclic stretch is not a replacement for `LoopRenderer`:
`LoopRenderer` repeats or crossfades a declared source region, while cyclic
stretch resynthesizes successive machine-domain cycles to change duration. It
is also not a replacement for conventional `signal::OfflineStretch`. Use the
latter for transparent tempo matching; use fixed/adaptive Heritage commit
stretch when cyclic behavior is deliberately part of the sound.

Profiles are strict schema-v3 JSON with neutral `neutral.*` IDs and canonical
import/export. Pulp ships several
[neutral example recipes](../../examples/PulpSampler/heritage-profiles/README.md),
but no named hardware profiles. Captures and listening are optional calibration
evidence for a profile, not requirements for using or extending the neutral SDK.
The public
[`heritage-profile` skill](../../.agents/skills/heritage-profile/SKILL.md)
includes templates and a reusable authoring prompt.

## Instrument and analysis policy

`InstrumentRuntime` resolves zone/pool triggers and playback rates. Add
`InstrumentVoiceAllocator` and `AhdsrEnvelope` only when their generic policy
matches the product. A sampler with distinct sustain, stealing, or automation
semantics can keep product-local voice state while still sharing traversal,
interpolation, mapping, and analysis primitives.

`SlicePointAnalyzer` keeps timeline-order/near-zero behavior by default. Its
selection options can instead choose the strongest deterministic candidates,
limit the resulting region count, and snap to sign transitions. Use
`SampleKeyMap::slice_index_for_note()` so slice triggering and full `SliceMap`
resolution share the same bounds.
