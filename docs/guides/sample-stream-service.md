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
registers sources, submits drained demands, and calls `service_once()`. The
audio callback may read prepared `SampleStreamWindow` views, but must not call
the file reader or service methods directly. RT-originated requests belong in a
prepared `SampleStreamRequestInbox` and are drained by the service owner.

`service_once()` decodes at most one canonical page. Multiple requesters for
that page produce one read. A requester cancellation removes only that voice's
future demand; another voice's demand remains. When no Empty page is available,
the request remains pending and `NoPageAvailable` is returned.

## Current limits

This is the synchronous core needed to test identity, budgeting, scheduling,
publication, and cancellation before adding concurrency. It does not yet own a
worker pool, completion mailboxes, page eviction, retired-page reuse, dynamic
source replacement, or an audio-thread command port. Only Empty page slots are
filled. Production multi-voice integration must add those pieces and prove
joinable teardown, generation-gated reuse, bounded queue behavior, and
resident-versus-streamed render parity before advertising general long-sample
streaming.

`StreamingSampleSource` remains the simpler preload-plus-ring utility for
sequential one-shot playback. It is not instantiated once per sampler voice.
