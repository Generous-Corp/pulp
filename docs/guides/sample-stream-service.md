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

`service_once()` decodes at most one canonical page. Multiple requesters for
that page produce one read. A requester cancellation removes only that voice's
future demand; another voice's demand remains. When no Empty page is available,
the request remains pending and `NoPageAvailable` is returned.

## Current limits

This is the synchronous core needed to test identity, budgeting, scheduling,
publication, cancellation, and generation-gated FIFO page reuse before adding
concurrency. It does not yet own a worker pool, completion mailboxes, or dynamic
source replacement. Production multi-voice
integration must add those pieces and prove joinable teardown, bounded queue
behavior, active-page interest, and resident-versus-streamed render parity
before advertising general long-sample streaming.

`StreamingSampleSource` remains the simpler preload-plus-ring utility for
sequential one-shot playback. It is not instantiated once per sampler voice.
