# Stamped WaveNet audio bridge

`GpuWaveNetRealtimeNode` is an experimental SDK node for using the existing
shared-memory WaveNet session through `GpuAudioTransport`. It owns model metadata,
dilation arrays and weights copied at construction. There are no public Dawn,
Metal, provider or queue handles.

An application derives from the node to implement `prime_fallback()` and
`process_cpu_fallback()`. These must advance the exact CPU model every callback
and return the output delayed by `lead_blocks * block_size` samples. Set
`miss_policy = CpuFallback` and `supports_cpu_fallback = true` only when both are
implemented. The default is silence. A transport miss never executes a worker CPU
model: the callback consumes its already prepared substitute. This removes a
second CPU evaluation, but retains the full CPU shadow and does not establish
CPU-offload savings.

## Lifecycle and thread contract

1. Construct and prepare while callback and worker are stopped. The config owns
   copies of all supplied spans. One existing `GpuWaveNetSession` is created for
   each channel. Explicit `prewarm_blocks` runs that many zero blocks before
   streaming; the application's CPU shadow must be warmed identically.
2. Prepare `GpuAudioTransport` with this prepared node. Its ring capacity must
   satisfy the transport's ordinary lead requirement. Failed preparation is a
   capability failure; this node cannot silently use the legacy sample FIFO.
3. The callback publishes a fixed planar record to `SharedIoStampedBridge` and
   consumes only the exact due epoch/sequence. It performs no Dawn calls,
   allocation, lock, encoding or completion wait. Normal fixed buffer copies
   remain at this callback bridge; shared provider memory does not mean every
   CPU-side copy is absent.
4. The serialized non-RT worker calls each provider service once per pump. It
   does not busy-wait. This first version permits one all-channel block in
   flight; only successful output from every channel publishes the original
   stamp. It is a bounded correctness architecture, not a throughput claim.
5. Stop and join both callers before `release()` or reprepare. A failed physical
   drain returns false and retains owners for retry. Destruction follows the
   existing session quarantine rules; retain the node when explicit retry is
   required.

`fenced()` is a read-only delivery-state snapshot. The capability report retains
the prepared adapter identity; it is not evidence that a particular callback
accepted GPU audio. Stop/join lifecycle operations before querying diagnostics
from another thread.

Late output cannot fill a later hole. A provider's late record is suppressed;
the stamped consumer also discards output whose audio position has passed.
Late output alone does not invalidate successfully advanced causal history.
Input loss, sequence gaps, submission rejection or provider failure do invalidate
history, so they disable GPU delivery for the entire preparation. The CPU shadow
continues. There is no automatic zero-history restart during a live signal.
Starting again requires stopped preparation and an explicit matching history
contract. Silence warmup is not restoration of the previous musical signal.

Offline processing fences provider work and continues the same aligned callback
fallback. GPU delivery stays disabled after returning to realtime until explicit
preparation. Do not reset CPU history merely because GPU output was fenced.

## Evidence and limits

`pulp-test-gpu-wavenet-realtime-node` uses private fake providers to force delayed,
failed, mismatched and partially successful channel completions. It checks leads
1/2/4/8, stale rejection, input loss, release retry, epoch replacement, callback
allocation observation, callback-only shadow execution, and rejection of the
legacy route. Fake paths report provider `Unknown`, never Dawn.

These tests prove sequencing and fallback contracts, not hardware deadline
reliability. An authenticated installed SDK build, actual GPU delivery controls,
matched DSP output, CPU cost and graphics/load contention remain separate gates.
The worker scheduling/completion policy still affects latency. There is no
hard-realtime GPU scheduling guarantee and no NAM speedup claim.

### Optional bounded completion servicing

`completion_service_wait_ns` is a non-realtime worker budget, capped at 1 ms,
and defaults to zero. Each worker pump computes one fresh monotonic deadline
and shares it across all channels. The audio callback never uses this budget.
The session must also select a supported `TimedWaitAny` completion policy;
`ProcessEvents` remains nonblocking. The budget may reduce completion polling
delay, but it is not an audio deadline and provides no hard GPU scheduling
guarantee.
