# GPU audio reliability on macOS

> Status: experimental, default-OFF. This page states what Pulp's GPU-audio
> runtime guarantees on macOS today, and where each guarantee stops. Work that
> is planned but not implemented is marked **Coming** and is not part of the
> current guarantee.

Read the [GPU audio runtime guide](gpu-audio-sdk.md) first for the transport
model. This page is about failure: what happens to audio when the GPU is slow,
hung or lost, how a host finds out, and what has been validated in real DAWs.

## The five reliability claims, at their current scope

Each claim below is worded at the scope Pulp has proven. The work that would
widen it is listed under each claim.

### 1. The audio callback never waits for the GPU

On the `GpuAudioTransport` path, the audio callback never waits on, locks
against, allocates for, or calls into the GPU. All provider work runs on a
non-realtime worker or service thread, and every wait there is bounded. The
bounds are not all audio-sized:

- the staged `GpuConvolver` worker does not wait on the GPU when a transport
  hosts it with a lead of two or more blocks. Called directly, or with a
  one-block lead, it waits at most one block period minus its CPU cost. The
  `StagedBlocking` escape hatch keeps the older in-block readback wait of up to
  2 s;
- `GpuMultiConvolver` and the spectral classes still read back synchronously,
  with the same 2 s bound;
- the Dawn shared-I/O provider bounds each drain at 15 s. The same bound covers
  the drain at the transport's offline fence (on the host's offline render
  thread), provider creation, and error-scope waits after a failed submission.

The callback is unaffected by these waits, because it reads a result a fixed
number of blocks later and falls back when the result is not there.

Not covered: nodes used outside the transport, and the explicitly offline
`process_offline()` path, are synchronous. Keep them off the audio thread.

`GpuNeuralRealtimeNode`'s CPU shadow may run on the audio callback, depending
on its shadow policy (see [shadow policy](#shadow-policy)). That is CPU work on
the callback, not GPU work.

`GpuAudioNode::release_bounded(budget_ns)` bounds teardown only on
`GpuWaveNetRealtimeNode`. A provider that cannot drain within the budget has
its device destroyed and stays owned (quarantined) until a later release
completes it, and the call returns `false`. Every other node keeps the default,
which holds nothing and returns `true`, so its teardown is not bounded.

**Coming:** bounded teardown for `GpuNeuralRealtimeNode` and the convolvers.

### 2. Device loss and hung providers are detected and fenced

Pulp's audio GPU provider owns its own Dawn device; it does not share the
editor's render device. Today:

- **Loss on the shared-I/O path.** `GpuWaveNetRealtimeNode`, the GPU path of
  `GpuNeuralRealtimeNode` (WaveNet is the only family with one) and the
  shared-I/O `GpuConvolver` session detect device loss through the provider's
  loss query, latch it and fence.
- **Loss on the staged path.** The staged `GpuConvolver` latches
  `ProviderLost` and routes every later block to its CPU convolution, so the
  output continues exactly. `GpuMultiConvolver` refuses a lost device and is
  silent, as its `Silence` policy declares. `GpuStft`, `GpuSpectralStack`,
  `GpuSpectralFreeze` and `GpuSpectralMorph` refuse a borrowed device that is
  lost or has a new generation, and write a silent frame.
- **Hang.** `GpuWaveNetRealtimeNode` and the GPU path of
  `GpuNeuralRealtimeNode` run a completion watchdog. Each latches
  `ProviderUnresponsive` when the provider completes nothing for
  `Config::watchdog_blocks` callback blocks while work is in flight. The
  default is 200 ms of blocks (150 at 64 frames and 48 kHz), or
  `capacity - 1` if that is larger. On `GpuWaveNetRealtimeNode`, a worker that is starved while its
  provider keeps completing is tested not to be reported as unresponsive.
- **Slow but alive.** The staged `GpuConvolver` trips a circuit breaker after a
  run of late results and moves to the CPU, reporting `SustainedDeadlineMisses`.
- **Fence.** On the stamped shared-I/O nodes (both WaveNet nodes and the shared
  `GpuConvolver`), any latched failure fences the current epoch. A stale or
  late GPU result is never delivered.
- **Recovery is manual by default.** After a fence or a loss, audio continues
  on the CPU path where the node has one, and on silence where it does not.
  Under the default `GpuAudioRecoveryPolicy::Manual`, GPU delivery stays off
  until the host releases and prepares the node again. The staged breaker is
  the one exception: it probes a slow, live GPU and re-enables it.
- **Opt-in re-acquire, `GpuWaveNetRealtimeNode` only.** With
  `GpuAudioTransport::Config::recovery_policy = AutoReacquire`, the transport's
  worker releases a fenced `GpuWaveNetRealtimeNode`'s provider, prepares a new
  one and opens a new delivery epoch on the same stream, while the callback
  stays on its fallback. An offline fence is never re-acquired. For any other
  node the transport's `capability_report().recovery_policy` reads `Manual`
  whatever is configured, and the node stays fenced. See
  [re-acquire a lost provider](gpu-audio-sdk.md#re-acquire-a-lost-provider-while-audio-keeps-playing).
- **Warm providers.** A host-owned `GpuAudioProviderPool`
  (`GpuWaveNetRealtimeNode::Config::provider_pool`) keeps released providers
  for `keep_warm_ns` (30 s by default), so a later `prepare()` of the same
  program borrows one instead of creating a device.

Not covered:

- a driver or process crash;
- render-device loss. The editor's render device is not linked to the audio
  provider. Render-owner loss propagation, resource replay, teardown callback
  suppression and authenticated render-loss DAW continuity are gated on the
  Vellum render-owner seam and are **not proven**;
- a hung provider behind the convolvers: only the two WaveNet GPU paths run a
  watchdog.

**Coming:** automatic re-acquire for `GpuNeuralRealtimeNode` and the
convolvers.

### 3. A missed GPU block is substituted without waiting

A block whose GPU result is not ready by its deadline is substituted from a
continuously primed, latency-aligned CPU path without waiting, for these nodes:

- `GpuConvolver`: the callback's continuously fed CPU convolution
  covers a block the worker did not deliver. On the staged path the worker also
  routes each late, failed, non-finite or device-lost block to its own CPU
  convolution, so the worker's output is the exact continuation of the
  convolution;
- `GpuNeuralRealtimeNode`: a built-in CPU shadow for every family (WaveNet,
  TCN, SSM, LSTM, GRU). Families without a GPU kernel run the same contract
  CPU-only and report it. Under the `Worker` shadow policy, a block that both
  the GPU and the shadow miss is silent and is counted in
  `shadow_double_miss_blocks`. Under `None`, a realtime GPU miss is silent;
- `GpuWaveNetRealtimeNode`: the CPU shadow your derived class supplies, once
  `cpu_fallback_prepared()` is true.

`GpuMultiConvolver` declares `Silence` on a miss. The spectral classes
(`GpuStft`, `GpuSpectralFreeze`, `GpuSpectralMorph`, `GpuSpectralMaskSession`,
`GpuSpectralStack`) are not `GpuAudioNode`s: they are synchronous, are not
hosted by the transport, and have no miss policy or status.

### 4. Failures are reported through the versioned status

Missed deadlines, fallback use, device loss, unresponsive-provider detection,
provider state and the first latched recovery reason are reported through
`GpuAudioStatus` (schema v4) for every transport-hosted node. Read them from
`GpuAudioTransport::status_snapshot()`. See [reading health](#reading-health)
for which fields the transport fills.

Not yet:

- **Trace.** The status is not emitted as Perfetto or JSONL records. The trace
  carries per-block terminal and delivery dispositions; see
  [the tracing guide](gpu-audio-tracing.md).
- **Control protocol and MCP.** There is no read operation for GPU-audio health
  yet. `dev.pulp.gpu/health.read@1` reports rendering health, not audio.

**Coming:** a GPU-audio health read operation for the control protocol and MCP.

### 5. The contracts carry no backend types

The node, transport, status, capability and program contracts contain no Dawn,
Metal or other backend types. On macOS there is one GPU provider, Dawn.
`GpuAudioProvider::Cpu` labels a node that runs CPU-only; it is not a
pluggable CPU implementation of the provider seam.

A provider conformance kit (`test/harness/shared_io_provider_conformance.hpp`)
drives a provider through the production shared-I/O arena and writes a
`pulp.gpu-audio.provider-conformance.v1` receipt. It runs against the test
fake and the Dawn provider. A host-memory CPU provider exists behind the seam
as a private implementation with its own tests; no node uses it, and the
conformance kit does not run against it.

**Coming:** a provider registry and a public CPU provider.

## Reading health

`GpuAudioTransport::status_snapshot()` is the UI-safe read. It is
allocation-free and never touches the node, so a host may re-prepare or destroy
the node while a UI thread reads status. The transport worker samples the
node's health after every service step or pump, so a snapshot can trail the
node by one pump.

The transport projects the lifecycle, the recovery reason and sequence, the
epoch, the delivery counters, `terminal_counts`, the `shadow_*` fields,
`gpu_kernel_available`, `completion_mode`, `timestamps_available` and
`last_delivery_ns` from the node's health sample. A field the node does not
report stays at its default.

The transport fills the schema v4 re-acquire fields itself:
`reacquire_attempts` counts provider replacements it attempted after a fence,
`reacquire_successes` counts attempts that opened a new delivery epoch, and
`last_reacquire_to_gpu_ns` is the time from first observing the fence to the
first GPU-delivered block of the replacing epoch (zero until measured). All
three stay zero under `Manual` and for a node that cannot re-acquire.

### Provider state and recovery reason

`provider_state` is the lifecycle summary. `recovery_reason` is the **first**
cause that latched, with `recovery_sequence` set to the stream position where it
latched (`UINT64_MAX` when nothing has latched).

| `recovery_reason` | `provider_state` | Produced by |
| --- | --- | --- |
| `None` | `Ready` (GPU delivering, or a CPU-only neural family), or `Degraded` (staged path, no provider, or a lagging GPU; see below) | every node |
| `SequenceGap`, `InvalidCallback` | `Degraded` | both WaveNet GPU paths and the shared `GpuConvolver` |
| `InputSaturated`, `OfflineFence` | `Degraded` | both WaveNet nodes and the shared `GpuConvolver` |
| `ProviderFailure` | `Degraded` | a failed provider completion; a staged `GpuConvolver` map failure or non-finite readback; a WaveNet model whose GPU provider could not be prepared |
| `ProviderLost` | `Unavailable` | both WaveNet GPU paths, both `GpuConvolver` paths, `GpuMultiConvolver` |
| `ProviderUnresponsive` | `Unavailable` | the watchdog on the two WaveNet GPU paths |
| `SustainedDeadlineMisses` | `Degraded` | the staged `GpuConvolver`'s circuit breaker |
| `ProviderReacquired` | informational; the new provider's own state applies | `GpuWaveNetRealtimeNode` under `AutoReacquire`, after a successful re-acquire |

Every node reports a device loss observed after a failure, a stall or
sustained misses (`ProviderFailure`, `ProviderUnresponsive` or
`SustainedDeadlineMisses`) as `ProviderLost`, both in its own status and in the
transport's. Independent causes (`SequenceGap`, `InputSaturated`,
`InvalidCallback` or `OfflineFence`) stay first: a node that latched one of
those and then lost its device keeps that reason and reports `Unavailable`.

A provider that keeps completing, but too late for the callback, latches no
cause. On the stamped shared-I/O nodes (`GpuWaveNetRealtimeNode`, the GPU path
of `GpuNeuralRealtimeNode` and the shared `GpuConvolver`), `Ready` on the `Gpu`
engine also requires the GPU to carry the recent stream. The status reads
`Degraded`, with `recovery_reason` `None`, when:

- at least 8 of the last 32 callback blocks were counted and the GPU delivered
  fewer than half of them; or
- at least one block was counted and the GPU delivered none of them, as during
  a first preparation's or a re-acquired epoch's warmup.

Priming blocks are not counted. The status returns to `Ready` once the GPU
carries half the window again, and a re-acquire starts a new window.

`ProviderReacquired` replaces the cause that fenced the old epoch, with
`recovery_sequence` set to the first block the new provider can deliver. The
replaced cause stays in the trace and in the `reacquire_*` counters.

On the staged `GpuConvolver` and `GpuMultiConvolver`, `recovery_sequence` is
the worker block at which the reason latched. A loss the worker has not reached
yet is attributed to the next block it will run.

On the shared path, `GpuConvolver::fenced()` reads `true` until the first
health sample, because an unsampled session is reported as `Degraded`. A
transport samples immediately at `prepare()`, so this only affects a convolver
read directly before it is hosted.

A CPU-only neural node (TCN, SSM, LSTM, GRU, or WaveNet built without a GPU
session) has nothing to fence. An invalid or out-of-sequence callback is
counted in `callback_contract_violations()` instead of being latched, so the
node stays `Ready` while its CPU output continues. On the GPU path the same
violation fences GPU delivery and is reported as the recovery reason.

### Which engine is delivering

`selected_engine` names the engine that actually produces the audio:

- `Gpu` only while the provider is `Ready` with a GPU provider, so not while a
  lagging GPU holds the status at `Degraded`;
- `Cpu` when a prepared CPU path covers misses, for a CPU-only family, and for
  a staged `GpuConvolver` whose breaker has tripped or whose device is lost;
- `Unknown` when misses are silent, or on the staged path while it uses the
  GPU, where a live device does not prove which engine produced a block.

A `Degraded` or `Unavailable` status never reports `Gpu`. `provider` still
names the provider that was prepared, so a CPU engine with a `Dawn` provider is
a valid combination.

### Counters

- `produced_blocks` is worker progress, not proof that the callback used a GPU
  result. Use `GpuAudioTransport::delivery_snapshot()` for what the callback
  actually selected.
- `missed_blocks` counts genuine GPU misses. It is not a synonym for
  `fallback_blocks`. A CPU-only node's blocks count under `fallback_blocks` and
  never under `missed_blocks`, because the CPU model is its primary path.
- `terminal_counts[]` counts admitted blocks by `GpuAudioTerminalDisposition`
  (index 1 is `GpuDelivered`, index 2 is `CpuFallback`). A block is counted
  once its audio is certain. Only `GpuWaveNetRealtimeNode` and the shared
  `GpuConvolver` fill them; the neural node, the staged convolver and
  `GpuMultiConvolver` leave them at zero. Read through the transport, they come
  from the node's health sample. The shared `GpuConvolver`'s own
  `status_snapshot()` fills them only while trace recording is enabled.
- `epoch` and `delivery_epoch_active` identify the GPU delivery epoch. A
  CPU-only node stays at epoch 0.
- `gpu_kernel_available` means a GPU kernel is prepared, not that it is
  usable. It is true for `GpuWaveNetRealtimeNode`, for the shared
  `GpuConvolver`, for a staged `GpuConvolver` that has a device, and for
  `GpuNeuralRealtimeNode` while its GPU backend is prepared, and false for a
  family with no GPU kernel. A staged `GpuConvolver` keeps it true after its
  device is lost, while its status reports `Unavailable`; read
  `provider_state` for usability.
- `completion_mode` and `timestamps_available` are reported only by
  `GpuWaveNetRealtimeNode`; other nodes leave `Unknown` and `false`. This
  provider path records no GPU timestamps, so `timestamps_available` is always
  false today.
- `last_delivery_ns` is reserved: the transport passes it through, but no node
  fills it.

### Shadow fields

`shadow_mode`, `shadow_inline_cost_pct`, `shadow_delivered_blocks`,
`shadow_late_blocks`, `shadow_double_miss_blocks` and `shadow_model_differs`
describe `GpuNeuralRealtimeNode`'s CPU shadow, and the transport passes them
through:

- `shadow_mode` is the policy in effect: `Inline`, `Worker` or `None`;
- `shadow_inline_cost_pct` is the measured total cost across channels, as a
  percentage of the block period;
- `shadow_delivered_blocks` counts blocks the shadow delivered in place of the
  GPU;
- `shadow_late_blocks` counts shadow results dropped because they arrived after
  their slot, in a slot the GPU also missed;
- `shadow_double_miss_blocks` counts blocks that both the GPU and the shadow
  missed. It is the authoritative count of silent blocks. The transport's
  `CpuFallbackDelivered` label means only that the fallback path ran;
- `shadow_model_differs` is set when a separate, cheaper `shadow_model` is in
  use, so the substitute does not null against the GPU output.

`GpuNeuralRealtimeNode::shadow_status()` adds the requested policy, per-channel
cost, queue drops, resyncs and workgroup membership.

### Validating a snapshot

`validate_gpu_audio_status(status)` checks a snapshot's internal consistency and
returns a typed `GpuAudioStatusError`. Among other things it rejects:

- a `Degraded` or `Unavailable` status that selects the GPU engine;
- `ProviderLost` or `ProviderUnresponsive` without `Unavailable`;
- `SustainedDeadlineMisses` without `Degraded`;
- `Ready` with a cause that fences delivery (`SequenceGap`, `InputSaturated`,
  `ProviderFailure`, `InvalidCallback` or `OfflineFence`);
- `Ready` on a stamped node (nonzero `epoch`) whose `delivery_epoch_active` is
  false;
- the GPU engine without `gpu_kernel_available`;
- a CPU provider with a GPU kernel or a GPU epoch;
- shadow counts without a shadow;
- terminal counts beyond the delivery counters;
- a recovery reason without its sequence, or a sequence without a reason;
- `CpuFallback` declared with no prepared fallback.

Validate a snapshot taken at a quiescent point. Counters sampled while a node
runs can be momentarily out of step, so a live caller should treat
`TerminalCountsInconsistent` as advisory. A status that validates is consistent;
it does not show that a ready provider met a realtime deadline.

## Per-architecture behaviour

| Model family | Node | GPU path | Miss handling | Reports |
| --- | --- | --- | --- | --- |
| WaveNet (NAM) | `GpuNeuralRealtimeNode` with `NeuralFamily::WaveNet` and a GPU session | Dawn shared-I/O | built-in CPU shadow, per `shadow_policy` | `gpu_kernel_available = true` while the backend is prepared; `ProviderLost`; watchdog |
| WaveNet (NAM) | `GpuWaveNetRealtimeNode` (derive from it) | Dawn shared-I/O | your CPU shadow | `gpu_kernel_available = true`; `ProviderLost`; watchdog |
| TCN, SSM, LSTM, GRU | `GpuNeuralRealtimeNode` | none | the CPU model is the output, always inline | path `Cpu`, provider `Cpu`, `selected_engine = Cpu`, `gpu_kernel_available = false`, epoch 0, `Ready`; callback-contract violations are counted, not latched |

- **No recurrent GPU kernels.** LSTM, GRU and SSM run one dependent step per
  sample, so a single-instance GPU kernel cannot beat the CPU at small blocks.
  They run CPU-only by design.
- **WaveNet without a provider.** When the GPU provider cannot be prepared,
  preparation still succeeds and the node runs CPU-only with an inline shadow.
  It reports `Degraded` with `ProviderFailure`, not `Ready`.
- **Non-finite input.** NaN and infinite samples are replaced with zero before
  either the GPU or the shadow sees them. `sanitized_input_samples()` counts them.
- **Recovery.** On the GPU path, any latched cause, including a single invalid
  callback, fences GPU delivery until the host prepares the node again.
- **Model construction.** Build a model on the host thread with
  `make_gpu_neural_model()` from `<pulp/gpu_audio/gpu_neural_model.hpp>`. You
  supply a provenance manifest, the family's shape and one
  `GpuNeuralCpuKernel` per channel; the kernels are the CPU model the node runs
  as its output or shadow. Only a WaveNet request may also carry a GPU session
  configuration; the factory refuses one for any other family. It never
  throws and reports a refusal as a `GpuNeuralModelError`. Each model binds to
  one node.

### Shadow policy

`Config::shadow_policy` decides where a GPU family's CPU shadow runs. It
governs only realtime GPU misses:

- `Inline` runs the CPU model on the audio thread for every block, even when
  the GPU delivers.
- `Worker` runs it on a dedicated non-realtime thread. The callback only copies
  input in and, on a GPU miss, copies the shadow's stamped result out. A late
  shadow result is dropped, never shifted. The thread joins
  `shadow_audio_workgroup` when one is given, and otherwise runs at
  `USER_INTERACTIVE` QoS.
- `None` makes a realtime GPU miss silent. The model keeps running on the
  shadow thread so that an offline render can continue it. It is rejected
  (`ShadowRequired`) for a family without a GPU kernel. While `None` is in
  effect, the node's own `status_snapshot()` and `capability_report()` report
  `fallback_policy = Silence` and `fallback_available = false`. During an
  offline render, which runs the shadow inline, they report `CpuFallback`. The
  transport's status still reports the descriptor's `CpuFallback` for such a
  node, so read the node's status or `shadow_mode` to see that misses are
  silent.
- `Auto`, the default, times one block of the CPU model at `prepare()`. It
  picks `Inline` when that cost times the channel count is at most
  `kAutoInlineMaxCostPct` (15%) of the block period, and `Worker` otherwise.

CPU-only families, and a WaveNet node whose provider could not be prepared,
always run `Inline`, because the CPU model is their output. An offline render
always runs the CPU model inline. When an offline render starts, a `Worker` or
`None` shadow finishes its queue and continues inline on the same timeline, so
a bounce does not go silent. The first realtime callback after the bounce
returns the model to the shadow thread, copying only the pending results, so
an over-budget model never runs on the audio thread after an offline render.

A `Worker` shadow for a large model can still miss: its result must arrive
within the lead. Inline, the shadow's cost is paid on the audio thread for every
block. Measure it for the model size and channel count you ship.

## Convolution

**Staged `GpuConvolver` (the default `Auto`/`StagedOnly` policy).**

- **Pipelining.** When a transport hosts the convolver with a lead of two or
  more blocks, the worker submits block N without waiting and judges it
  `latency_blocks - 1` calls later. The node declares that as
  `worker_pipeline_blocks`, and the transport primes that many fewer blocks, so
  total latency and the reported delay compensation are unchanged. Called
  directly, or with a one-block lead, it decides each block inside the call
  with a bounded wait. The transport's `release()` opts a pipelining
  convolver back out; one destroyed before that call detaches itself from the
  transport first, and a node that does not pipeline is never touched there.
  That holds only once neither `pump()` nor `process()` runs on its transport:
  the audio callback's `process()` calls the node's CPU fallback, and a
  transport with its own worker pumps it until `release()` joins the worker.
  Otherwise release that transport before destroying the node.
- **Exact routing.** A block whose result is on time and finite uses the GPU.
  Any other block goes to the worker's CPU convolution, so the output is the
  exact continuation of the convolution, never silence. A miss costs one CPU
  block plus the IR's tail; with a long IR that tail is long.
- **Circuit breaker.** After 8 consecutive misses the convolver switches to the
  CPU. It probes the GPU every 32 blocks and re-enables it after 4 on-time
  results. Set different values with `configure_staged_breaker()` before
  `prepare()`. A trip reports `SustainedDeadlineMisses`, `Degraded`, and the
  CPU engine.
- **Status.** A failed map or non-finite readback latches `ProviderFailure`. A
  lost device latches `ProviderLost` (`Unavailable`, CPU engine) and replaces
  any other cause. A single late block latches nothing.

`ProviderPolicy::StagedBlocking` keeps the older in-block readback wait and is
a transition escape hatch.

**Shared-I/O `GpuConvolver` (`SharedRequired`).** This path runs on the
authenticated Dawn provider and reports `Ready` with provider `Dawn` while it
delivers. After the transport's offline fence, the session stays CPU-only until
the next preparation; it does not re-prime onto the GPU.

**`GpuMultiConvolver`.** No CPU fallback: a miss or a lost device is silent,
and the status says so with `fallback_policy = Silence`. `device_lost()`
reports why.

## Known host behaviours

These come from pre-merge runs of the REAPER continuity harness and from
the out-of-process probe used to design the multi-host matrix. Neither is
merged evidence. Each is a host fact to plan around, not a Pulp guarantee.

- **REAPER anticipative FX starves the GPU path.** With anticipative FX
  processing enabled, REAPER calls the plug-in in back-to-back bursts, so a
  pipelined GPU result is never ready in time and every block comes from the
  CPU fallback. Audio stays correct, but the GPU does no useful work. Turn
  anticipative FX off for the track to test the GPU path. A processing quantum
  smaller than the host block has the same effect.
- **The GPU does not resume after an audio fault (under the default `Manual`
  policy).** After any injected provider fault, delivery stays on the CPU
  fallback for the rest of the run. This matches the manual recovery policy
  described in claim 2.
- **No real host has shown a named loss reason yet.** In those runs, which used
  a code base from before the v3 status work, `recovery_reason` stayed `None`
  after a loss. The code now latches a reason; a real-host run against it has
  not been done.
- **AUHostingService.** On Apple Silicon, an AUv3, or an AU whose
  architecture does not match the host, is hosted out of process in
  AUHostingService, which is sandboxed and launched by launchd. A native arm64
  AUv2 normally loads in process. Out-of-process native AU hosting has not been
  validated. A probe through the Rosetta path (`AUHostingServiceXPC_arrow`)
  stayed on the CPU fallback without silence after an injected loss. That
  probe is design evidence, not a validated result.
- **Process-wide Dawn state.** A Dawn proc-table override affects every Dawn
  client in the process, including other plug-ins that share an
  AUHostingService process. The editor's render surface also reinstalls Dawn's
  native proc table each time it initializes.

### What has been validated in real DAWs

Nothing on this page has a merged real-DAW receipt yet.

| Host | Status |
| --- | --- |
| REAPER 7.82, CLAP and VST3 | Pre-merge harness runs exercised only the shared-I/O `GpuConvolver`, built with experimental build flags. They passed for injected loss, a 100 ms completion stall, a 3 s worker stall (the GPU kept completing, so this is not a test of hang detection) and editor render-device loss, with the plug-in's own captured output sample-exact against a CPU reference. They ran on a host that was not quiet. The VST3 matrix is partial, and a run with anticipative FX on was inconclusive. One early stall run produced a near-silent offline bounce, recorded as a verified FAIL; it did not reproduce in two later runs and is unexplained. |
| Logic Pro, GarageBand, Bitwig Studio | Not run. |
| AUv2 and AUv3 | Not run in a DAW. |

A PASS from that harness means the plug-in's own capture stayed sample-exact
across one injected fault. The REAPER bounce is an offline render made
afterwards, so it checks only the offline path and delay compensation. A PASS
does not prove recovery, hang detection, render-owner loss propagation,
resource replay or teardown suppression.

## Testing

- **Unit and contract tests.** The GPU-audio suites drive fake providers through
  loss, hang, lateness and fencing, and every status they produce passes
  `validate_gpu_audio_status`. They prove the sequencing and fallback contracts,
  not hardware deadline reliability.
- **Real DAW.** Use the [real-DAW smoke harness](daw-smoke.md). Its
  `--mode gpu-fault` injects one GPU fault into the test plug-in in
  `examples/gpu-fault-continuity/` while REAPER renders the track, and writes a
  `pulp.gpu-audio.daw-continuity.v1` receipt that
  `tools/validation/verify_gpu_audio_daw_continuity_receipt.py` checks. The
  other hosts' runs write a `v2` receipt, checked by
  `verify_gpu_audio_daw_continuity_receipt_v2.py`. Run it
  with anticipative FX off for the GPU path, and record the setting either
  way. No real-DAW receipt from it is merged yet.
- **Provider conformance.** The kit in
  `test/harness/shared_io_provider_conformance.hpp` runs the same contract
  scenarios against the test fake and the Dawn provider, and
  `tools/validation/verify_gpu_audio_provider_conformance.py` checks its
  receipt.
- **Dawn versus native Metal comparator.** **Coming:** a matched comparator for
  the provider decision. Its first campaign was inconclusive on a loaded host.

## Related guides

- [GPU audio runtime](gpu-audio-sdk.md): transport, nodes, capability reports.
- [Stamped WaveNet audio bridge](gpu-audio-wavenet-realtime.md): WaveNet
  lifecycle, loss and watchdog details.
- [Diagnosing GPU audio with Perfetto](gpu-audio-tracing.md): per-block trace
  dispositions.
- [Real-DAW smoke](daw-smoke.md): the REAPER harness.
