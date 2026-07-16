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
uncompressed AIFF, forward linear one-shots, two decode workers, and a bounded
eight-page working set for each of eight independently positioned voices. Its
service thread owns file/source/cache mutation; the callback owns only voice
state and the SPSC producer. The Loop parameter remains resident-only in this
gate. Streamed loops, reverse playback, starvation gain shaping, interpolation
quality selection, and mip assets remain later gates.

`SampleAsset` accepts streamed tails only through a service-issued registration
proof whose source identity and page geometry match the prepared cache. The
borrowed asset and source views remain valid only until their owners cross the
documented audio-generation retirement watermark. `SampleStreamVoiceReader`
is the first callback-side consumer: it provides allocation-free linear forward
one-shot reads, bounded coalesced demand, and explicit ready/starved/end/stale
results. Looping, reverse playback, and starvation gain policy remain separate
layers.

`StreamingSampleSource` remains the simpler preload-plus-ring utility for
sequential one-shot playback. It is not instantiated once per sampler voice.
