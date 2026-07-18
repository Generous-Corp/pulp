# Shared sample stream service

`pulp::audio::SampleStreamCacheService` is the deterministic service core for
sharing decoded sample pages across voices. It canonicalizes page geometry from
registered source metadata, coalesces voice demands for the same source page,
publishes through `SampleStreamWindow`, and rejects source-generation mismatches.

The service has a hard page-storage budget. Admission uses checked arithmetic:

```text
source page bytes = channels * page frames * cache page count * sizeof(float)
```

`add_source()` fails before playback when the cumulative reservation would
exceed `page_memory_budget_bytes`. The budget covers page sample storage; source
metadata, reader state, and control-thread containers are outside that value.

## Thread boundary

The cache core remains caller-driven. Production users can compose it with
`SampleStreamDecodePool` through `SampleStreamAsyncService`: one non-audio owner
registers sources, drains `SampleStreamCommandInbox`, dispatches reservations,
publishes completions, and collects retired sources. The audio callback may
read prepared `SampleStreamWindow` views and push bounded demand/cancellation
commands, but must not call the file reader or service methods directly. Inbox
overflow is explicit and never blocks; the producer retries commands according
to voice/source generation.
If a requester or pending-demand cancellation cannot be queued, its owner keeps
the token and retries before reuse; failure may waste bounded decode work but
cannot make an older requester generation satisfy a newer voice. Source memory
reclamation uses the non-RT `retire_source_after_asset_unpublish()` watermark
path, not a lossy inbox command. The asset publisher must first stop issuing
the old borrowed view; the service then captures the current audio generation
instead of accepting a caller-invented retirement number.

`service_once()` decodes at most one canonical page. Multiple requesters for
that page produce one read. A requester cancellation removes only that voice's
future demand; another voice's demand remains. When no Empty page is available,
the request remains pending and `NoPageAvailable` is returned.

## Async integration and current limits

`SampleStreamDecodePool` supplies fixed workers, preallocated planar scratch,
one in-flight decode per source, bounded SPSC mailboxes, cooperative stop
propagation, and leased completion audio. `SampleStreamAsyncService` binds those
workers to the cache with registration epochs and reservation serials. Queue
pressure retains the exact Filling reservation for retry, other sources may
still dispatch, and stale/error completions cancel only their matching fill
before the scratch lease is released. Release joins decode workers before cache
windows are destroyed.

`PulpSampler` is the first production-shaped integration: strict ranged WAV and
uncompressed AIFF, pitched one-shots and forward/reverse crossfade loops,
two decode workers, and a bounded prepared-page demand footprint for each of
eight independently positioned voices. Its
service thread owns file/source/cache mutation; the callback owns only voice
state and the SPSC producer. File admission prepares the certified tail horizon
before it publishes the asset, which gives reverse entry a latency-safe attack
neighborhood. Because those pages remain ordinary cache entries, a later
reverse note rechecks the full horizon and holds its cursor and envelope at
time zero until the first render plan owns valid snapshots for every attack
page. Admission deadlines scale with the number of sequential page decodes in
that horizon; timeout and shutdown paths cancel and reclaim unpublished
registrations. The
resident renderer now delegates traversal to the storage-independent
`LoopPlaybackCursor`, whose plans are checked against an independent loop
oracle. `SampleStreamLoopVoiceReader` snapshots every primary, blend, and
interpolation page needed by a block, advances the musical cursor through an
explicit miss, and supplies a cursor-based lookahead scheduler whose initial
lead is derived from the certified service, block, interpolation, and loop
guards. Forward notes demand their first nonresident boundary directly, so
one-frame host blocks retain the full service interval without enumerating the
resident horizon. Lookahead scanning has a fixed eight-plan work budget per
callback, and page urgency includes the accumulated distance
from the live render cursor rather than only the offset inside one plan. That
distance remains signed under command-queue backpressure, so lookahead catches
up from a real lag instead of claiming a false zero lead. Partial queue retries
refresh their accepted prefix at the current distance before adding the suffix.
Reverse entry, loop policy, and interpolation quality are exposed by
`PulpSampler`. Hold, true nearest, linear, cubic Hermite, cubic Lagrange, and a
ratio-tracking windowed-sinc tier share one prepared footprint/evaluation
policy across resident and paged playback. The sinc tier uses immutable,
off-callback Kaiser tables whose cutoff narrows with source consumption and
blends adjacent cutoff tables during modulation. If a resident playback ratio
exceeds the prepared table range, `PulpSampler` falls back to cubic Hermite
instead of dropping the voice. Each phase row is normalized for DC unity, and
the streamed preload contract must cover the selected kernel's complete tap
guard. Persisted streamed octave mips are loaded from strict `.pulpmip`
sidecars, and the voice reader applies a 64-frame equal-power fade to silence
and recovery ramp around starvation. See the
[PulpSampler example](../examples/pulp-sampler.md) for the current policy and
integration limits.

`SampleAsset` accepts streamed tails only through a service-issued registration
proof whose source identity and page geometry match the prepared cache. The
borrowed asset and source views remain valid only until their owners cross the
documented audio-generation retirement watermark. `SampleStreamVoiceReader`
provides the narrow linear forward path; `SampleStreamLoopVoiceReader` adds
allocation-free cursor-driven one-shot, reverse, prepared interpolation-tap, and
wrap-crossfade page planning. Both return explicit ready/starved/end/stale
results. Starvation gain policy remains a separate layer, implemented by
`SampleStarvationEnvelope` in the PulpSampler integration.

`StreamingSampleSource` remains the simpler preload-plus-ring utility for
sequential one-shot playback. It is not instantiated once per sampler voice.
