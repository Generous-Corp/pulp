# Stamped WaveNet audio bridge

`GpuWaveNetRealtimeNode` is an experimental SDK node for using the existing
shared-memory WaveNet session through `GpuAudioTransport`. It owns model metadata,
dilation arrays and weights copied at construction. There are no public Dawn,
Metal, provider or queue handles.

An application derives from the node to implement `prime_fallback()`,
`process_cpu_fallback()`, and `cpu_fallback_prepared()`. The witness must return
true only after the exact CPU model has been allocated and warmed during the
host-thread prepare step. The fallback methods must advance that model every
callback and return the output delayed by `lead_blocks * block_size` samples.
If a derived node overrides `prepare()`, it must allocate and warm this CPU
shadow before delegating to `GpuWaveNetRealtimeNode::prepare()`; the base
implementation samples `cpu_fallback_prepared()` during its preparation and
rejects the node if the witness is still false.
Set `miss_policy = CpuFallback` and `supports_cpu_fallback = true` only when
the witness and both methods are implemented. The default is silence. A
transport miss never executes a worker CPU model: the callback consumes its
already prepared substitute. This removes a second CPU evaluation, but retains
the full CPU shadow and does not establish CPU-offload savings.

## Lifecycle and thread contract

1. Construct and prepare while callback and worker are stopped. The config owns
   copies of all supplied spans. A derived `prepare()` must finish allocation
   and warmup of its CPU shadow, including the configured zero-block warmup,
   before calling the base implementation. The base then creates one existing
   `GpuWaveNetSession` for each channel, samples the witness, and rejects an
   unprepared fallback. Its provider-side `prewarm_blocks` must match the
   shadow's warmup so both histories start at the same position.
2. Prepare `GpuAudioTransport` with this prepared node. Its ring capacity must
   satisfy the transport's ordinary lead requirement. Failed preparation is a
   capability failure; this node cannot silently use the legacy sample FIFO.
3. The callback publishes a fixed planar record to `SharedIoStampedBridge` and
   consumes only the exact due epoch/sequence. It performs no Dawn calls,
   allocation, lock, encoding or completion wait. Normal fixed buffer copies
   remain at this callback bridge; shared provider memory does not mean every
   CPU-side copy is absent.
4. The serialized non-RT worker services existing completions before admitting
   one new all-channel block. With `completion_service_wait_ns > 0`, successful
   submission on every channel permits one additional completion pass in the
   same pump. There are at most two service passes per channel, sharing the
   original unchanged deadline; the second pass adds no wait budget and admits
   no further work. The default zero budget retains one service pass per pump.
   The worker does not busy-wait and permits one all-channel block in flight;
   only successful output from every channel publishes the original stamp.
   This is a bounded correctness architecture, not a throughput claim.
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

## Late results, device loss and hung providers

A result is late only when the callback that would consume it has already
finished. Lateness is measured in stream position, not wall-clock time, so
bursty host callbacks or a worker that wakes late never drop a result that can
still reach its callback. A late result retires as `LateRejected` with reason
`DeadlineExceeded` and is never delivered; like any late record it leaves causal
history intact and is a miss, not a provider fault.

When a provider reports that its device is lost, the node latches
`GpuAudioRecoveryReason::ProviderLost` at the oldest block still in flight.
Loss is checked when completions are collected and when a submission is
refused. Loss is the root cause of the failed completions and the silence it
produces, so once it is observed it replaces an earlier `ProviderFailure` or
`ProviderUnresponsive`, even one latched on an earlier worker pump. An
independent earlier cause, such as `InputSaturated`, stays the reported reason.

A watchdog on the worker latches `ProviderUnresponsive` when the provider
completes nothing for `watchdog_blocks` callback blocks (default
`capacity - 1`) while work is in flight. Silence is counted from the later of
the last completion and the oldest in-flight admission, in callback blocks
rather than worker pumps, because the worker wakes on a poll interval. The
default window ends before the input ring saturates as long as the worker keeps
up with the callback, so a hung provider is reported as unresponsive rather
than as `InputSaturated`. A provider that keeps completing, however late, is
slow, not unresponsive: its results retire as late, and if it falls behind for
good the input ring saturates. If the worker itself stops, the callback latches
`InputSaturated` when the ring fills.

`status_snapshot()` reports `provider_state = Unavailable` after an observed
device loss, whatever cause latched first, and for `ProviderUnresponsive`:
delivery cannot resume until the node is released and prepared with a new
provider. Every other recovery reason reports `Degraded`. `selected_engine`
names the engine actually producing audio: `Gpu` only while the provider is
`Ready`; otherwise `Cpu` when a prepared CPU shadow covers misses, or `Unknown`
when misses are silent. The schema v3 fields report the preparation `epoch`,
whether its delivery epoch is still active, `terminal_counts` by
`GpuAudioTerminalDisposition`, the configured `completion_mode`,
`gpu_kernel_available = true`, and `timestamps_available = false`, since this
provider path records no GPU timestamps. A terminal is counted once the audio
it names is certain: `GpuDelivered` by the callback that consumed the block,
and a provider failure under the declared miss policy, `CpuFallback` only after
its consumer delivered the fallback. A quiescent snapshot therefore passes
`validate_gpu_audio_status()`.

## Evidence and limits

`pulp-test-gpu-wavenet-realtime-node` uses private fake providers to force delayed,
failed, mismatched and partially successful channel completions. It checks leads
1/2/4/8, stale rejection, input loss, release retry, epoch replacement, callback
allocation observation, callback-only shadow execution, rejection of the
legacy route, device-loss precedence and escalation, the hung-provider watchdog
against slow providers, position-based late retirement, and the v3 status
fields. Fake paths report provider `Unknown`, never Dawn.

These tests prove sequencing and fallback contracts, not hardware deadline
reliability. An authenticated installed SDK build, actual GPU delivery controls,
matched DSP output, CPU cost and graphics/load contention remain separate gates.
The worker scheduling/completion policy still affects latency. There is no
hard-realtime GPU scheduling guarantee and no NAM speedup claim.

### Optional bounded completion servicing

`completion_service_wait_ns` is a non-realtime worker budget, capped at 1 ms,
and defaults to zero. Each worker pump computes one fresh monotonic deadline
and shares it across all channels and both completion passes, when the second
pass is enabled. Submission time consumes that same budget: the deadline is
never extended after submission. The audio callback never uses this budget.
The session must also select a supported `TimedWaitAny` completion policy;
`ProcessEvents` remains nonblocking. The budget may reduce completion polling
delay, but it is not an audio deadline and provides no hard GPU scheduling
guarantee.
