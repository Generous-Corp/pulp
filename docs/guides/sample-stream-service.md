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

The current service is deliberately caller-driven. A single non-audio owner
registers sources, drains `SampleStreamCommandInbox`, and calls
`service_once()`. The audio callback may read prepared `SampleStreamWindow`
views and push bounded demand/cancellation commands, but must not call the file
reader or service methods directly. Inbox overflow is explicit and never
blocks; the producer retries commands according to voice/source generation.
If a requester or pending-demand cancellation cannot be queued, its owner keeps
the token and retries before reuse; failure may waste bounded decode work but
cannot make an older requester generation satisfy a newer voice. Source memory
reclamation uses the non-RT `retire_source()` watermark path, not a lossy inbox
command.

`service_once()` decodes at most one canonical page. Multiple requesters for
that page produce one read. A requester cancellation removes only that voice's
future demand; another voice's demand remains. When no Empty page is available,
the request remains pending and `NoPageAvailable` is returned.

## Current limits

This is the synchronous core needed to test identity, budgeting, scheduling,
publication, cancellation, and generation-gated FIFO page reuse before adding
concurrency. It does not yet own a worker pool, completion mailboxes, or dynamic
source replacement orchestration. Sources can be scheduled for collection
behind the completed-audio watermark, but the asset publisher must still stop
new voice acquisition and reclaim matching asset views in the same order.
Production multi-voice integration must add the worker pieces and prove
joinable teardown, bounded queue behavior, active-page interest, and
resident-versus-streamed render parity
before advertising general long-sample streaming.

`SampleAsset` accepts streamed tails only through a service-issued registration
proof whose source identity and page geometry match the prepared cache. The
borrowed asset and source views remain valid only until their owners cross the
documented audio-generation retirement watermark. `SampleStreamVoiceReader`
is the first callback-side consumer: it provides allocation-free linear forward
one-shot reads, bounded coalesced demand, and explicit ready/starved/end/stale
results. Looping, reverse playback, worker dispatch, and starvation gain policy
remain separate layers.

`StreamingSampleSource` remains the simpler preload-plus-ring utility for
sequential one-shot playback. It is not instantiated once per sampler voice.
