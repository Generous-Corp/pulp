# GPU audio runtime (developer guide)

> Status: experimental, default-OFF. Requires a Skia/Dawn GPU build
> (`-DPULP_ENABLE_GPU=ON`). Most paths carry a CPU fallback so plugins keep
> working on unsupported devices; the ones that cannot fall back say so and fail
> to silence rather than to wrong audio.

This is **not** "run your audio on the GPU." It's a runtime that lets a plugin
selectively accelerate *computationally expensive* DSP on the GPU **while
keeping the audio callback bounded and preserving seamless CPU compatibility
where the node declares a fallback**. The audio thread is never blocked on the
GPU. A node with a declared fallback can use it when the GPU cannot do the work
or cannot finish before its prepared lead; nodes without one follow their
declared miss policy.
The callback contract is real-time-safe; GPU scheduling itself is not a hard
real-time guarantee. This guide is the developer surface: the compute
primitives, the real-time transport, and the ready-made processors.

For what happens when the GPU is slow, hung or lost on macOS, how a host reads
it, and what has been validated in real DAWs, see
[GPU audio reliability on macOS](gpu-audio-macos-reliability.md).

## The model in one paragraph

The runtime has two transport costs to manage: payload movement and scheduling.
The ordinary staged path uploads and reads back each block. On supported Apple
Silicon/Dawn configurations, the experimental shared-memory path imports
persistent host allocations and lets CPU and GPU operate on the same storage,
so the per-block CPU-to-GPU upload and GPU-to-CPU readback disappear. CPU-side
planar-to-complex packing, overlap-add, and callback buffer copies still exist.
Both paths still depend on non-real-time GPU submission, scheduling, and
completion observation. The audio thread publishes
work and reads a result a fixed number of blocks later (reported to the host as
plugin delay compensation); it never waits for the GPU. To make that pay off,
keep resources resident, fuse or batch enough work, and prepare explicit lead
instead of blocking. Small, low-latency DSP stays on the CPU, with a continuously
prepared fallback wherever one is tractable.

## Honest tradeoffs (read this first)

GPU audio is easy to oversell, so here are the hard parts up front — three things
to know before reaching for it:

1. **The fixed cost is real and we don't hide it.** The staged path pays for
   upload and readback. The Apple shared-memory path can remove those copies,
   but it does not remove submission, scheduling, completion observation, cache
   traffic, or contention. For small or scalar work those fixed costs can still
   dwarf the compute, and the GPU is then **slower** than the CPU. GPU work pays
   off when it is large or fused enough to amortize the fixed cost (long IRs,
   many voices/IRs batched, big FFTs, or neural inference). That's why
   SuperConvolver **defaults to the CPU engine** and the GPU engine is opt-in,
   aimed at the heavy regime.

2. **It is NOT a free speed-up for any plugin.** If your DSP is gain, biquads,
   a compressor, a small delay, an envelope follower, or MIDI — keep it on the
   CPU; the GPU will only make it slower. The win is narrow and specific (see the
   list below). We'd rather tell you that than sell you a GPU mode that
   regresses your plugin.

3. **Compatibility is opt-out-safe, and where it is not, it fails closed.** A
   common disappointment with GPU audio is a plugin that won't even run on a
   given card. Most Pulp paths avoid that with a `signal::*` CPU fallback.

   Not all of them can. `GpuMultiConvolver` (many IRs batched into one GPU
   submit) and Spectral Lab's cloud node have no tractable RT-safe CPU
   equivalent, so they declare `supports_cpu_fallback = false` and emit silence
   on a miss. That is deliberate: a bounded, obvious dropout beats plausible
   wrong audio. Check the processor's descriptor rather than assuming a
   fallback exists.

   For a node that DOES declare one: if the GPU is absent, unsupported, or fails
   to initialize, the node logs it and runs its `signal::*` CPU reference — the
   plugin still loads and produces correct audio, just unaccelerated. On a *miss*
   mid-stream (the worker didn't finish in time) the `CpuFallback` policy fills
   the block on the CPU seamlessly — but only after the host-thread
   `cpu_fallback_prepared()` witness confirms that the exact fallback state is
   built, and only if that fallback is kept continuously primed and
   latency-aligned (see `prime_fallback()` below); an unprimed one substitutes a
   stale block with a torn history. The `supports_cpu_fallback` descriptor bit
   is a declaration; it does not admit the policy by itself.
   `capabilities().backend` tells you at runtime
   which backend you actually got ("Metal"/"D3D12"/"Vulkan"), and audio paths are
   only **validated on Apple Silicon / Metal** today (see the matrix below) —
   everything else is experimental-but-falls-back. We won't claim a card works
   until we've tested it.

Bottom line: GPU audio here is a **latency-tolerant accelerator for heavy,
batched, parallel DSP**, not a hard GPU dependency or a hard scheduling
guarantee. Where a CPU fallback is tractable a node carries one; where it is
not, the node says so and fails to silence rather than to wrong audio.

## What belongs on the GPU (and what doesn't)

The GPU only pays off for work that is **heavy, parallel, and latency-tolerant** —
enough arithmetic per block to dwarf the CPU↔GPU round-trip. Match the workload.

**The value case — two ways GPU audio is genuinely worth it:**
1. **Things that are otherwise impossible / infeasible on CPU.** A single plugin
   whose compute a CPU can't sustain in real time: large-scale physical
   modeling, thousands of simultaneous convolutions/partials, room/spatial
   modeling that touches enormous sample counts, real-time neural inference. The
   most compelling case is a plugin that simply *couldn't exist* on a CPU at all
   — that's where the GPU earns its keep.
2. **Headroom — running more heavy work at once.** Offloading demanding,
   batchable DSP to the otherwise-idle GPU so the session can carry more heavy
   instances/voices than the CPU alone would allow.

**When it is NOT worth it (we'll say it plainly):** if your plugin is light DSP —
a basic synth, an EQ, a delay — leave the GPU out; there's little to gain and the
round-trip will only make it slower. And because the GPU path is **latency-
tolerant by design** (it adds fixed plugin-delay-compensated latency), it suits
mixing, sound-design and rendering — *not* zero-latency live tracking/monitoring.

The crossover, concretely: GPU wins once per-block compute ≫ the round-trip, and
the win grows as you scale size (longer IRs, more voices, bigger FFTs/banks).
Below that line the CPU is faster — so pick per workload:

**Good GPU candidates** (big parallel math, or batched across instances/voices):
- FFTs / iFFTs
- Partitioned & long-IR convolution (and many IRs at once)
- Spectral EQ, spectral freeze, spectral morphing
- Large filter banks / beamforming
- ML model inference (NAM / CLAP-style neural models)
- Waveform & spectrogram generation; large additive / sinewave banks

**Keep on the CPU** (too small to beat the round-trip; latency-sensitive):
- Simple gain, biquad filters, compressors
- Small delays, envelope followers, MIDI processing
- Recurrent neural models (LSTM, GRU) — a deliberate choice, see below

**Why recurrent models stay on the CPU.** A recurrent layer's state at sample
*t* depends on its state at *t − 1*, so a block cannot be computed in parallel
across time the way a feedforward WaveNet block can: the GPU would walk the block
one sample at a time inside a persistent kernel. That only pays off when many
streams are batched side by side to fill the GPU; for one stream, the GPU round
trip alone costs more than running the small recurrent model on the CPU. Pulp's
GPU neural path is therefore feedforward-only (WaveNet), and LSTM/GRU captures
run on the CPU engine. Revisit this only with a batched multi-stream design and a
measured win over the CPU at the stream counts that matter.

Rule of thumb: if it's a tight per-sample scalar op, leave it on the CPU. If it's
a block-wide transform or a bank you can batch, the GPU can win — and the
further you push size (long IRs, many voices, big FFTs), the bigger the win.
SuperConvolver's GPU mode targets the long-IR / many-IR regime for this reason;
at short IRs the CPU path is selected because it's faster there.

## Which GPUs / platforms

GPU compute runs through the WebGPU layer (Dawn / wgpu-native), so it follows
WebGPU's backends. Where a node declares and prepares a CPU fallback, a plugin
still works with no compatible GPU — it just runs the `signal::*` reference
path. Where it declares none (`supports_cpu_fallback = false`), or where the
host-thread `cpu_fallback_prepared()` witness is false, a missed or unavailable
GPU block comes out silent, never as wrong audio.

| Platform | Backend | GPUs |
|---|---|---|
| macOS — Apple Silicon | Metal | experimental; use only with an exact provider/machine receipt |
| macOS — Intel | Metal | Intel / AMD GPUs |
| Windows 10+ | D3D12 (or Vulkan) | NVIDIA, AMD, Intel |
| Linux | Vulkan | NVIDIA, AMD, Intel |
| no compatible GPU | — | CPU fallback (correct, not accelerated) |

`capabilities().backend` reports the live backend at runtime ("Metal" / "D3D12" /
"Vulkan"), plus limits and optional features (timestamp-query, f16). Audio paths
require exact provider/machine validation on Apple Silicon / Metal; the other
backends are supported by the WebGPU layer but not yet audio-validated — treat
them as experimental until benchmarked, and rely on the CPU fallback meanwhile.

## Layer 1 — compute primitives (`pulp::render::GpuCompute`)

Create with `GpuCompute::create()` then `initialize_standalone()` (own device,
headless) or `initialize_from_surface(GpuSurface&)` (share the UI device and
queue for compute work). Device sharing does not by itself make audio/UI buffers
zero-copy. `capabilities()` reports backend, limits, and optional features
(timestamp-query, f16). All are validated against CPU references.

| Primitive | Method | Use |
|---|---|---|
| FFT / iFFT | `fft_forward` / `fft_inverse` (+ `fft_forward_timed`) | spectral analysis, fast convolution |
| Fused convolution | `prepare_convolution` + `convolve` | one-readback FFT convolution with a resident IR |
| Batched convolution | `prepare_convolution_batch` + `convolve_batch` | many blocks/instances in one submit+readback (amortizes per-dispatch + readback overhead; measured ~2× at batch 2 / stereo — the multiplier grows with batch size until it becomes bandwidth- or readback-bound) |
| Magnitude / complex-mul | `compute_magnitude`, `complex_multiply`, `batch_magnitude` | spectral building blocks |
| Dense matmul | `matmul` | neural layers, matrix DSP (ambisonics, mixing) |
| Dense layer | `dense_tanh` | neural inference (NAM dense / LSTM gates) |
| WaveNet | `prepare_wavenet` + `wavenet_forward` | feedforward dilated-conv neural amp inference; one submit + readback per block, history kept on the GPU across blocks |
| WaveNet batch | `wavenet_forward_batch` | several prepared instances in one submit and one readback (any mix of shapes) |
| WaveNet group | `prepare_wavenet_group` + `wavenet_forward_group` | many same-shape instances in shared buffers: one dispatch per pass for the whole group, so the per-block command count does not grow with the instance count. `pulp-gpu-wavenet-scaling-bench` measures per-block wall, CPU and GPU-busy time for the three paths |
| Additive synth | `additive_synth` | sum of sinusoidal partials |
| Modal synth | `modal_strike` | struck/plucked resonant-mode banks |
| Granular | `granular_cloud` | windowed pitch-shifted grain clouds |

These are **not real-time-safe** (they block on readback); call them from the
worker (Layer 2) or offline.

## Layer 2 — real-time transport (`pulp::gpu_audio`)

`GpuAudioTransport` bridges the audio thread and a non-RT worker. Implement a
`GpuAudioNode` (descriptor with channels/block/latency/miss-policy; `prepare()`;
`process_block()` on the worker; `process_cpu_fallback()` RT-safe). `prepare()`
the transport with the node and `Config{ ring_blocks, run_worker_thread }`; the
audio callback calls `process()` — it writes the input ring and reads the
latency-delayed output, never blocking. On a miss the node's `MissPolicy`
fills the block — `Silence` by default (a bounded, obvious dropout), or
`CpuFallback` if you have a fallback you keep primed and latency-aligned. **Report
`latency_samples()` to the host**: the transport delays your output by
`latency_blocks * block_size`, and a host that is not told leaves your track
shifted late against every other track in the session.

The transport also has an opt-in macOS scheduling experiment for an owned
worker. Set `Config::join_audio_workgroup` and pass a borrowed
`Config::audio_workgroup` handle when the host has a valid `os_workgroup_t`.
The worker joins before it starts pumping and leaves during `release()`. The
callback still only publishes a ring record and can use `wake_on_write`; it
never joins a workgroup or calls Dawn. `Stats::worker_workgroup_joined` and
`worker_workgroup_join_failures` report adoption. This is a scheduling
experiment, not a hard realtime claim: Dawn command encoding, submission, and
completion APIs may allocate and lock internally, so the submission path stays
off the callback and the CPU fallback remains continuously prepared. The
handle is borrowed and must outlive the transport.

`GpuConvolver` exposes a host-thread-only provider policy for experiments that
need an explicit selection: `Auto` preserves staged execution, `StagedOnly`
forces the staged provider, `StagedBlocking` keeps the older blocking staged
readback (see below), and `SharedRequired`
fails `prepare()` unless the exact authenticated shared Dawn provider is ready.
Set the policy before `prepare()`; changing it while prepared is rejected. This
is a preparation capability contract, not a realtime scheduling guarantee, and
it exposes no Dawn handles, queues, rings, or callback controls.

**Staged convolver readback (behaviour change).** Under `Auto` and `StagedOnly`
the staged `GpuConvolver` no longer blocks in `convolve_batch` until the map
resolves, and its worker never waits on the GPU at all.

- The worker submits block N with `GpuCompute::convolve_batch_async` and
  returns. Block N's result counts only if it has landed by the time block N
  is due, `latency_blocks - 1` worker calls later; a result that lands later is
  discarded. The node declares that pipeline as
  `GpuAudioNodeDescriptor::worker_pipeline_blocks` (1 at the default lead of
  2), and `GpuAudioTransport` primes that many fewer blocks, so total latency,
  the reported PDC and the callback's CPU-fallback alignment are unchanged. A
  larger lead therefore tolerates a slower readback: with a lead of 3 a result
  may take up to two block periods. A node with a one-block lead cannot
  pipeline and instead waits inside the block, bounded to one period minus its
  CPU cost.
- The pipeline is used only while a `GpuAudioTransport` hosts the node; the
  transport opts the node in at `prepare()` and out at `release()`. Called
  directly, `process_block` still returns block N for input block N and
  reports `worker_pipeline_blocks = 0`, deciding each block inside the call
  with a wait bounded to one period minus its CPU cost (with the same CPU
  routing and breaker). Because `release()` opts a pipelining convolver back
  out, that node must outlive the transport's `release()` (or the transport
  must be released first); a non-pipelining node is never touched there.
- Every block's input goes to exactly one place: the GPU overlap-add carry when
  its readback is on time, otherwise the worker's zero-latency CPU convolution.
  The output is the carry plus whatever the CPU convolution still owes, so a
  late, failed, non-finite or device-lost block is the exact continuation of
  the convolution, never silence.
- The CPU convolution runs only for a missed block and its IR-length tail
  (`ceil((ir_length - 1) / block)` further blocks); a GPU hit costs no CPU
  convolution. With a long IR one miss therefore costs a long CPU tail (a 2 s
  IR at 64-sample blocks: about 1500 blocks), which is the price of an exact
  seam.
- A run of late results trips a circuit breaker (`GpuConvolverStagedBreaker`,
  set with `configure_staged_breaker()` before `prepare()`; defaults: trip
  after 8 consecutive misses, probe every 32 blocks, recover after 4 on-time
  results). While tripped the worker submits only the probes.
- The node latches the first cause since `prepare()`: `ProviderLost` (device
  loss; replaces any other cause), `SustainedDeadlineMisses` (the breaker
  tripped on a slow but alive GPU; reported as degraded), `ProviderFailure`
  (failed map or non-finite readback). A single late block is a served miss
  and latches nothing. `prepare()` now also rejects a zero sample rate.

A caller that depends on the old unbounded, in-block wait can select
`ProviderPolicy::StagedBlocking` before `prepare()` (no worker pipeline); it
still routes failed or device-lost blocks to the CPU convolution.
`StagedBlocking` is a transition escape hatch and is planned for removal in the
next minor release.

The Darwin arm64 SDK produced by `release-cli.yml` compiles the shared convolver
and validates its pinned Dawn provider at configure and before library builds. Installed CMake
exports `PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO`,
`PULP_GPU_AUDIO_SHARED_CONVOLVER_ENABLED` and
`PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF_ENABLED` as producer capability facts.
They do not establish runtime availability or change a plugin's default engine.
The release provenance binds these facts to the installed config and GPU-audio
archive; older SDKs may not contain these facts. Traced/probe builds and the
Vellum D15 experiment remain outside the official release contract. The legacy
`release-cli-local.sh` helper does not produce this qualified capability receipt;
its archive is not a substitute for the official release artifact.

The required arm64 macOS gate also configures `PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON`,
so CI compiles and links the hardware-evidence binaries (for example
`pulp-gpu-dawn-wavenet-qualification` and the shared-I/O provider probes)
against the pinned Dawn on every pull request and merge group. It never runs
them: their tests carry the `gpu-proof` label, which every CI ctest selection
excludes. Hardware receipts remain owner-run on a physical GPU (m3), and a
claim-bearing change still attaches a fresh receipt from that machine.

Earlier experimental builds let `Auto` try shared execution when the convolver
compile flag was enabled. Experiments relying on that behavior must now request
`SharedRequired` before preparation. Private diagnostic trial overrides retain
their explicit selection. WaveNet and spectral sessions do not depend on the
convolver flag; they still require a supported shared provider.

The ordinary staged transport copies between its CPU rings and the provider.
The experimental shared path instead keeps provider-owned slots persistently
imported from host-visible storage. The callback still publishes and consumes
bounded records; a non-RT service owns submission and completion. Shared memory
therefore removes per-block CPU/GPU staging transfers, not all CPU-side copies
and not the scheduling problem. Choose
enough fixed lead for the measured workload and keep the CPU fallback ready for
late, stale, failed, or unavailable GPU results.

The installed SDK also exposes the backend-neutral
`<pulp/gpu_audio/gpu_audio_program.hpp>` contract for the next class of
providers. A `GpuAudioProgramDescriptor` identifies a typed workload
(`Convolution`, `Spectral`, `Neural`, or `Custom`) and records the fixed stream
shape, lead, bounded pipeline depth, provider-slot count, provider identity,
and whether the CPU fallback and persistent provider-owned resources were
prepared. `validate_gpu_audio_program()` applies the same fail-closed rules to
every provider. It is metadata and preparation validation only: it deliberately
does not expose Dawn or Metal handles, queues, rings, or callbacks, and it does
not by itself activate a generic shared provider. Shared execution requires a
concrete SDK-owned adapter, such as `GpuConvolver` or the experimental
`GpuWaveNetRealtimeNode`.

The installed SDK also includes `<pulp/gpu_audio/gpu_wavenet.hpp>`. Its
`GpuWaveNetDescriptor` is the narrow, model-neutral admission boundary for a
one-stream WaveNet model, and `GpuWaveNetSession` is the corresponding opaque
model/session lifecycle seam. A session accepts the descriptor and flat weights,
keeps the authenticated provider resources and causal history resident, and
offers block submission, non-blocking service, completion, and quiescent
release without exposing Dawn or Metal handles. The first session requires
`stream_instances == 1` and contiguous sequence numbers. Its submission and
service methods are serialized non-realtime operations; it does not turn GPU
scheduling into a hard-realtime contract. Consumers must keep a continuously
prepared CPU fallback for unavailable, failed, or late blocks.

`<pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>` adds a narrow stamped adapter
for these sessions. Derive from `GpuWaveNetRealtimeNode` to retain a continuously
primed, aligned CPU shadow without a second worker CPU model. Provider failures
and missing results remain tied to their epoch/sequence; they never shift later
samples into an earlier hole. See [the lifecycle and limitations](gpu-audio-wavenet-realtime.md).
This is experimental correctness infrastructure, not a CPU-saving claim.

`<pulp/gpu_audio/gpu_neural_realtime_node.hpp>` adds `GpuNeuralRealtimeNode`,
one realtime contract for every neural family Pulp hosts. The family
(`NeuralFamily::WaveNet`, `Tcn`, `Ssm`, `Lstm`, `Gru`, `Custom`) is a
configuration field, not a class. Every node owns a built-in CPU shadow of its
model. The transport feeds the shadow on every callback, and the shadow emits
its result `lead_blocks` callbacks later, so `cpu_fallback_prepared()` is true
for every prepared node and the miss policy is always `CpuFallback`. Only
WaveNet has a GPU backend. TCN, SSM, LSTM and GRU run the same contract on the
CPU only: their `capability_report()` reports the `Cpu` path and provider with
`gpu_kernel_available` false, and `epoch()` stays zero because no GPU delivery
epoch is ever opened. A WaveNet model whose provider cannot be prepared uses the same
CPU-only contract. Its status reports a GPU request that was degraded to the
CPU, and preparation still succeeds. Build a model with
`make_gpu_neural_model()` from `<pulp/gpu_audio/gpu_neural_model.hpp>`. Pass
it a provenance manifest, the family's shape, and one `GpuNeuralCpuKernel`
per channel. For WaveNet you can also pass a GPU session configuration. The
factory reports why it refused a request and never throws. Each model binds
to one node.

On the GPU path the node detects device loss through the provider's loss
query, which outranks the failed completions the loss causes. It detects a
hung provider with a completion watchdog: `Config::watchdog_blocks` callback
blocks with nothing completed while work is in flight. Both report
`Unavailable`, with `ProviderLost` or `ProviderUnresponsive` as the reason,
and the CPU model carries the stream.

Where the shadow runs is `Config::shadow_policy`. The policy governs only
what happens on a realtime GPU miss:
- `Inline` runs the CPU model on the audio thread for every block, even when
  the GPU delivers.
- `Worker` runs it on a dedicated non-realtime thread. The callback only copies
  its input in and, on a GPU miss, copies the shadow's stamped result out. A
  shadow result that arrives after its slot is dropped. A block that both the
  GPU and the shadow miss is silent.
- `None` makes a realtime GPU miss silent. The model is still kept running on
  the shadow thread, so an offline render can continue it. It requires a
  family with a GPU kernel. While it is in effect the node's own status and
  capability report `fallback_policy = Silence` with no fallback available;
  the transport's status still reports the descriptor's `CpuFallback`.
- `Auto`, the default, times one block of the CPU model for one channel at
  `prepare()`. It picks `Inline` when that cost times the channel count is at
  most 15% of the block period, and `Worker` otherwise.

Families without a GPU backend always run `Inline`, because the CPU model is
their output. So does a GPU family whose provider could not be prepared.

An offline render always uses the CPU model inline, whatever the policy. When
an offline render starts, a `Worker` or `None` shadow finishes its queue and
continues inline on the same timeline, so a bounce never goes silent. The
first realtime callback after the bounce hands the model back to the shadow
thread, so realtime runs with the placement chosen at `prepare()` again. That
switch only copies the pending results, and it never allocates or waits.

`status_snapshot()` reports the shadow in the public `shadow_*` fields of
`GpuAudioStatus`: the mode, the total inline cost, and how many blocks were
delivered, late or double-missed. `shadow_double_miss_blocks` is the
authoritative count of silent blocks. The transport's `CpuFallbackDelivered`
delivery label means only that the fallback path ran, which includes a
double miss that emitted silence. `shadow_status()` adds the requested
policy, the per-channel cost, queue drops, resyncs and workgroup membership.

A cheaper `shadow_model` is allowed but never selected by default, and status
reports `shadow_model_differs` when one is in use. Set
`shadow_audio_workgroup` to the host audio thread's workgroup so the shadow
thread joins it as a member doing audio-deadline work. Without one, the thread
runs at `USER_INTERACTIVE` QoS.

A `Worker` shadow for a large model can still miss blocks. Its result must
arrive within the lead. For a model that costs about half the block period, a
lead of two blocks leaves little slack, so a loaded host will see occasional
double misses.

A transport hosting a CPU-only family reports the `Cpu` path and provider and
a `Ready` state. Because the CPU model is that node's primary path, its blocks
count under `fallback_blocks` and never under `missed_blocks`. A WaveNet
model whose GPU provider could not be prepared also runs on the `Cpu` path. It
reports `Degraded`, with `ProviderFailure` as the recovery reason.

Non-finite input samples (NaN, +Inf, -Inf) are replaced with zero before
either the GPU submission or the CPU shadow sees them, so the two stay
comparable and no causal state is poisoned. `sanitized_input_samples()`
counts the replacements.

Any latched cause, including one invalid callback, fences GPU delivery until
the host prepares the node again. The shadow carries the stream until then.
`last_prepare_error()` reports why `prepare()` failed.

When a host has already prepared a transport, pass its
`GpuAudioCapabilityReport` to the two-argument overload of
`validate_gpu_audio_program()`. That overload binds the declaration to the
prepared path, provider identity, lead, and fallback policy. A consumer cannot
turn a self-authored `provider_owned_resources` flag into a shared execution
claim when the host reports staged, unavailable, ineligible, or differently
prepared execution. The check is constexpr, allocation-free, and still does
not expose provider resources.

Every future typed provider should also assign exactly one
`GpuAudioTerminalDisposition` to each admitted block (`GpuDelivered`,
`CpuFallback`, `Silence`, `StaleRejected`, `LateRejected`, `DeviceLost`, or
`Cancelled`). Keeping that outcome in the backend-neutral contract makes it
possible to connect block IDs and terminal delivery to the existing trace and
Perfetto work without deriving the result from a loose collection of events.

For host diagnostics, `capability_report()` returns an allocation-free snapshot
of the selected path. `path` distinguishes the ordinary staged worker from the
experimental shared-memory path, while `provider` is `Dawn` only when the exact
shared Dawn path is active and is otherwise `Unknown`. The snapshot carries
`prepared_latency_blocks` for the transport's fixed output/PDC delay. It carries
`prepared_lead_blocks` only when an authenticated concrete provider establishes
an algorithmic lead; generic staged nodes report zero there. It also carries the
miss policy, fallback availability, and whether transport diagnostics are
available. `GpuConvolver::backend()` reports the
underlying native backend (`Metal` for this Dawn path), while `provider` reports
the API implementation (`Dawn`). `Eligible` means the path was accepted by
`prepare()`; it is not a hard real-time scheduling guarantee. The report is
read-only and deliberately exposes no rings, queues, callback hooks, or live
path-switching controls.

At present, the authenticated shared provider has concrete `GpuConvolver` and
one-stream `GpuWaveNetSession` integrations, with `GpuWaveNetRealtimeNode` providing
a stamped transport adapter for the latter. A custom `GpuAudioNode` passed to
`GpuAudioTransport` uses the staged path unless it is implemented by a Pulp-owned
shared-provider adapter;
the generic node API cannot opt into shared execution merely by reporting a
Metal backend. This is intentional. A future generic shared-program contract
must carry provider identity, prepared resources, fallback behavior, and
terminal delivery diagnostics without exposing Dawn or Metal handles to SDK
consumers. Until that contract exists, downstream neural or spectral plugins
should treat `GpuAudioTransport` as a staged compatibility path and must not
claim UMA shared execution from `capability_report()` alone.

When a capability change affects the installed GPU-audio surface, the required
macOS ARM64 build publishes an exact SDK artifact named
`pulp-gpu-audio-sdk-<tested-sha>-macos`. The archive contains the installed
`PulpConfig.cmake`, public headers, libraries, and a receipt with the source
SHA and per-file hashes. A downstream consumer should unpack that artifact and
pass the receipt's `source_sha` to its installed-SDK validator. A build-tree
compile or a target-presence check is not a substitute for this artifact: the
consumer must bind its result to the exact tested SDK prefix before claiming
compatibility.

## Layer 3 — ready-made processors (`pulp::gpu_audio`)

- `GpuConvolver` — FFT overlap-add convolution with a fixed IR; a continuously-fed, zero-latency `signal::FixedIrConvolver` CPU fallback that stays latency-aligned so a GPU miss is filled seamlessly.
- `GpuStft` — STFT/ISTFT (windowed analyze / synthesize) — the spectral toolkit base.
- `GpuSpectralFreeze` — capture + sustain a spectral frame (infinite pad).
- `GpuSpectralMorph` — blend between two captured spectra (t ∈ [0,1]).

`GpuStft`, `GpuSpectralStack`, `GpuSpectralFreeze` and `GpuSpectralMorph` may
borrow a shared `GpuCompute`. Each records the device's `device_generation()`
at `prepare()` and refuses its GPU work (returns false and writes a silent
frame) once that device reports `device_lost()` or a different generation;
`device_usable()` (`available()` on the stack) reports the refusal. Prepare
them again against the re-acquired device to resume. `GpuMultiConvolver`
refuses a lost device the same way and keeps its declared `Silence` policy;
`device_lost()` says why its blocks are silent.

## Authoring pattern

1. Pick the layer: a ready processor (Layer 3), or your own `GpuAudioNode` whose
   `process_block` calls Layer-1 primitives.
2. Always implement a CPU fallback (`signal::*`) — it's your degrade path on
   no-GPU devices and your miss policy.
3. Keep small/low-latency DSP on the CPU; send only coarse, batched, heavy work
   to the GPU, and batch across instances/voices to amortize the round-trip.
4. Test against a CPU reference (golden / frequency-domain) and assert RT-safety
   (no alloc/lock/block on the audio thread).

### A minimal GPU node, end to end

A `GpuAudioNode` is the whole integration surface. Implement four methods — what
you are, how to set up, the worker-side work, and an RT-safe fallback — then let
the transport schedule it:

```cpp
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/render/gpu_compute.hpp>

using namespace pulp;

// 1. The node: heavy, batched work that runs on the transport's worker thread.
class MyGpuNode : public gpu_audio::GpuAudioNode {
public:
    MyGpuNode(uint32_t channels, uint32_t block, uint32_t sr)
        : channels_(channels), block_(block), sr_(sr) {}

    gpu_audio::GpuAudioNodeDescriptor descriptor() const override {
        gpu_audio::GpuAudioNodeDescriptor d;
        d.name = "MyGpuNode";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_;
        d.sample_rate = sr_;
        d.latency_blocks = 1;   // fixed PDC — you MUST report this (see below)

        // Miss policy. Both fields default to failing CLOSED (Silence, no
        // fallback): a missed block comes out silent. That is the right default —
        // a dropout is obvious and bounded.
        //
        // Opt into CpuFallback only if you actually have a correct fallback. It
        // is not enough to implement process_cpu_fallback(): a stateful fallback
        // (a convolver, a filter) must ALSO be fed every block via prime_fallback()
        // so its history is current and latency-aligned the moment it takes over.
        // An unprimed fallback substitutes a stale block with a torn history.
        //
        // Do NOT use MissPolicy::PassthroughDry. The transport's output is delayed
        // by latency_blocks, so substituting the dry sample for the CURRENT time
        // jumps the stream forward by the whole latency for one block and back
        // again — a timeline break, not an honest dropout.
        d.miss_policy = gpu_audio::MissPolicy::CpuFallback;
        d.supports_cpu_fallback = true;
        return d;
    }

    // Host-thread witness: the allocation-free fallback is built and warmed
    // before the transport may admit CpuFallback.
    bool cpu_fallback_prepared() const noexcept override { return prepared_; }

    // Non-RT setup: create the device + upload static data here, never in process_block.
    bool prepare() override {
        prepared_ = false;
        gpu_ = render::GpuCompute::create();
        if (!gpu_ || !gpu_->initialize_standalone()) { gpu_.reset(); return false; }
        // ... prepare Layer-1 plans (FFTs, convolution, matmul) on gpu_ ...
        // ... build and warm the CPU fallback, then mark it prepared ...
        prepared_ = true;
        return true;
    }

    // Worker thread: may touch the GPU and block on readback. NEVER the audio thread.
    void process_block(const audio::BufferView<const float>& in,
                       audio::BufferView<float>& out, uint32_t n) override {
        // ... call gpu_->fft_forward / convolve / matmul, write `out` ...
    }

    // RT-safe: called on EVERY block (hit or miss), before the output-ring read,
    // so a stateful fallback stays continuously fed and its substitute is a
    // correct, latency-aligned continuation of the stream rather than a stale
    // block with gaps in its history. Required whenever your fallback has state.
    void prime_fallback(const audio::BufferView<const float>& in,
                        uint32_t n) noexcept override {
        // ... advance your signal::* CPU path by one block and stage its output ...
    }

    // RT-safe: runs on the audio thread for a missed block. No alloc/lock/block.
    void process_cpu_fallback(const audio::BufferView<const float>& in,
                              audio::BufferView<float>& out, uint32_t n) noexcept override {
        // ... emit the substitute prime_fallback() already computed for this slot ...
    }
private:
    uint32_t channels_, block_, sr_;
    bool prepared_ = false;
    std::unique_ptr<render::GpuCompute> gpu_;
};

// 2. Wire it into your Processor. prepare() once; process() per block, RT-safe.
class MyPlugin : public format::Processor {
    MyGpuNode node_{2, 256, 48000};
    gpu_audio::GpuAudioTransport transport_;

    void prepare(const format::PrepareContext&) override {
        node_.prepare();
        transport_.prepare(&node_, {/*ring_blocks*/ 8, /*run_worker_thread*/ true});
    }

    // REPORT THE LATENCY. The transport delays your output by
    // latency_blocks * block_size, and the host cannot know that unless you say
    // so — it will leave your track shifted late against every other track in
    // the session, and nothing will error. This override is not optional.
    int latency_samples() const override { return transport_.latency_samples(); }

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in, /*...*/) override {
        transport_.process(in, out, out.num_samples());  // writes input, reads delayed output — never blocks
    }
};
```

Prove that `latency_samples()` is honest rather than trusting it — the number is
hand-maintained and drifts the moment the block size or `latency_blocks` changes:

```bash
pulp audio render --plugin MyPlugin.clap --out /tmp/o.wav --duration-ms 500 \
    --input-signal noise --param <your-bypass-id>=1 --latency-report /tmp/lat.json
```

It exits nonzero if the reported latency does not match the delay actually in the
audio. See [the CLI reference](../reference/cli.md#proving-reported-latency---latency-report).

That is the entire contract: the audio thread only ever calls `transport_.process()`
(lock-free), the GPU work happens on the worker a fixed number of blocks behind,
and a missed block degrades via your `MissPolicy` instead of glitching.

### Worked examples

Both ship as buildable plugins (VST3/AU/CLAP/Standalone) with a CPU default and
an opt-in GPU engine, a CPU-reference correctness test, and a benchmark:

- **`examples/super-convolver`** — multi-room convolution reverb. Shows batching
  *many distinct IRs* into one GPU submit (`GpuMultiConvolver`), the live CPU↔GPU
  engine switch, and a live status line (backend, blocks, µs/block, real-time
  headroom). The clearest read for the transport + live-rebuild lifecycle.
- **`examples/spectral-lab`** — N-layer spectral freeze/morph "cloud". Shows
  authoring your own `GpuAudioNode` (`gpu_spectral_cloud_node.hpp`) over a
  GPU-resident spectral stack, and reusing the same status-line + headroom
  telemetry. The clearest read for writing a node from scratch.

## Building against an installed Pulp SDK

Everything above works whether Pulp is a source submodule or an installed SDK.
A standalone plugin repo that consumes Pulp through `find_package(Pulp)` (rather
than `add_subdirectory`) gets the GPU audio runtime from the SDK's exported
targets:

| You link | You get |
|---|---|
| `pulp::gpu-audio` | Layer 2 transport + Layer 3 ready-made processors (`GpuAudioTransport`, `GpuConvolver`, `GpuStft`, `GpuSpectralFreeze`/`Morph`) and their headers |
| `pulp::gpu-device` | Layer 1 primitives (`GpuCompute`, `GpuSurface`) without the Skia 2D renderer — present only in a GPU-enabled SDK build. `pulp::gpu-audio` links this, not `pulp::render` |
| `pulp::render` | The Skia 2D renderer, which also carries the Layer 1 primitives — present only in a GPU-enabled SDK build |

`pulp::gpu-audio` is always exported: it's GPU-agnostic and carries its own CPU
fallback, so a non-GPU SDK still exposes the transport (it just runs the
`signal::*` reference path). `pulp::gpu-device` and `pulp::render` exist only
when the SDK was built `-DPULP_ENABLE_GPU=ON`, so gate any GPU-primitive code on
the device target:

```cmake
find_package(Pulp CONFIG REQUIRED)

pulp_add_plugin(MyPlugin
    FORMATS      VST3 AU CLAP Standalone
    SOURCES      my_plugin.cpp
    PLUGIN_NAME  "MyPlugin"
    # ...
)

target_link_libraries(MyPlugin_Core PUBLIC pulp::gpu-audio)
if(TARGET pulp::gpu-device)
    target_link_libraries(MyPlugin_Core PUBLIC pulp::gpu-device)  # Layer-1 GPU primitives
endif()
```

`find_package(Pulp)` resolves the transport's private dependencies for you
(`Threads`, and `ICU` on Linux) — a static library propagates its private link
deps into its exported interface, so `PulpConfig` re-finds them; you don't list
them yourself.

### Ship a self-contained GPU bundle

A plugin built with Skia's Dawn (the normal GPU build) does not load
`libwgpu_native.dylib` and needs no WebGPU runtime beside it. A build without
Skia's Dawn falls back to wgpu-native and loads `libwgpu_native.dylib` at
runtime. From a source build the
WebGPU dependency copies that dylib next to each format binary automatically; an
installed-SDK consumer has no such upstream copy step, so a `find_package(Pulp)`
plugin must place the dylib itself and bake an `@loader_path` rpath — otherwise
the bundle passes every check on the build machine and fails to load
("Library not loaded: @rpath/libwgpu_native.dylib") on any other Mac. The SDK
exports two helpers for exactly this. `find_package(Pulp)` already defines them —
no extra `include()` — so call them directly:

```cmake
foreach(_fmt MyPlugin_CLAP MyPlugin_VST3 MyPlugin_AU MyPlugin_Standalone)
    if(TARGET ${_fmt})
        pulp_make_bundle_relocatable(${_fmt})       # copy dylib + bake @loader_path
        pulp_validate_bundle_relocatable(${_fmt})   # POST_BUILD guard: fail the build if not
    endif()
endforeach()
```

`pulp_validate_bundle_relocatable` runs at build time and fails hard if a bundle
still references a build-machine-absolute path, so the footgun is caught in CI
instead of on a customer's machine. Apply it to single-bundle formats
(Standalone/CLAP/VST3/AU); an AUv3 `.appex` is intentionally not relocatable in
isolation — its dylib and framework live in the container app's
`Contents/Frameworks` and resolve by `@rpath` only inside that container, so the
container app (not the standalone appex) is the relocatability unit.

GPU work runs outside the audio callback, with CPU fallback when its result is
unavailable. This bounds the callback's interaction with the GPU; it does not
provide a hard scheduling guarantee from the operating system or driver.

For the experimental shared-memory route, the [paced convolution
probe](gpu-audio-paced-probe.md) records callback timing, missed deliveries,
and numerical correctness through the public transport and its own worker.
Use the [tracing guide](gpu-audio-tracing.md) for per-block lifecycle analysis.

### Verify the output selected by the audio callback

`GpuAudioTransport::delivery_snapshot()` reports cumulative, allocation-free
counters for prepared `process()` calls. Each call increments exactly one field:
`gpu_blocks`, `worker_output_blocks`, `cpu_fallback_blocks`, `silence_blocks`,
`passthrough_blocks`, `priming_blocks`, or `invalid_blocks`.

Use these counters when testing an installed plugin. A successful submission or
worker completion does not prove the callback used that result. `gpu_blocks`
counts ready output selected through the callback-side GPU path. Generic worker
ring output is counted separately as `worker_output_blocks`: a generic node can
perform CPU work or internal fallback, so a ring delivery cannot establish GPU
execution. Pair the counters with the capability report and an audio reference
comparison; counters alone establish neither numerical correctness nor a speedup.

Offline and unprepared calls are excluded. Invalid prepared calls count once,
even when the transport sanitizes their views. Successful preparation resets the
counters; release and preparation returning false preserve them. A failed
preparation leaves the transport unprepared.

Snapshots use independent atomic loads and may combine different callback
instants while processing continues. Stop the callback before checking exact
aggregate totals. Do not race preparation or destruction with snapshot readers.
These aggregate counters complement the per-block tracing identities; they do
not replace terminal-disposition or deadline analysis.

### Read provider health after a fence

`GpuAudioTransport::status_snapshot()` folds the hosted node's health into
`provider_state`. A node whose GPU delivery is fenced reports `Degraded` with the
first latched `recovery_reason` and `recovery_sequence`, for example
`InputSaturated`, `ProviderFailure`, `OfflineFence` or `InvalidCallback`. While it
is `Degraded`, the callback continues on the prepared CPU fallback, or emits
silence for a node that declared no fallback. A lost provider (`ProviderLost`, or
`ProviderUnresponsive`) is `Unavailable`, because nothing in the current
preparation can bring it back. A loss observed after the `ProviderFailure`,
`ProviderUnresponsive` or `SustainedDeadlineMisses` it caused is reported as
`ProviderLost`. A node that latched an independent earlier cause, such as
`InputSaturated`, still reports `Unavailable` and keeps that cause. Whenever
the state is not `Ready`, `selected_engine` names the engine that actually delivers. That is
`Cpu` for a node with a CPU fallback, and `Unknown` for a node that emits
silence; it is never `Gpu`. `provider` still names the provider that was
prepared. `epoch`,
`delivery_epoch_active` and `terminal_counts` come from the node when it
supplies them. The shared `GpuConvolver` path supplies the epoch and counts one
terminal disposition per admitted GPU block. A refused or failed block is counted
as `CpuFallback`, because that is what the callback delivers for it.

The read is allocation-free and safe from a UI thread. `status_snapshot()` and
`capability_report()` read only the transport's own copies: the configuration
captured at `prepare()`, and the last health sample. They never touch the node,
so a host may re-prepare or destroy the node while a UI thread reads status.
The transport worker takes the samples. For `GpuWaveNetRealtimeNode` and the
shared `GpuConvolver` path, it calls the node's private health hook after every
service step. For a staged node, it calls the node's own
`GpuAudioNode::status_snapshot()` after every pump. `prepare()` and the offline
fence sample as well. Each sample is republished as one coherent copy, so a
snapshot can trail the node by up to one worker pump but never mixes two
samples. If no stable copy is available for a realtime node, the status is never
reported as `Ready`. A staged node whose status keeps the base default carries
no health, so the transport's staged `Degraded` stands.

The convolver family reports the same status directly. `GpuConvolver`,
`GpuMultiConvolver` and `GpuConvolutionReverb` each gain `status_snapshot()`,
`recovery_reason()` and `fenced()`. A staged convolver reports `provider` and
`selected_engine` as `Unknown`, because a live `GpuCompute` device identifies
neither. Without a device, the engine is reported as `Cpu`, and so is a
tripped breaker or a lost device, because the worker's CPU convolution then
delivers every block. A lost staged device is `Unavailable` with `ProviderLost`,
and `recovery_sequence` is the worker block at which the cause latched. `GpuMultiConvolver` declares
`fallback_policy == Silence` and `fallback_available == false`, so a lost device
explains silent blocks. `GpuConvolutionReverb::status_snapshot()` returns the
least healthy lane, and `report().lanes[i].status` carries each lane.

Check a quiescent snapshot with `validate_gpu_audio_status()`. The
[reliability page](gpu-audio-macos-reliability.md#reading-health) lists which
node produces each recovery reason today, and which values are reserved with no
producer yet.

### Keep host controls separate from callback execution

Musical parameters retain the plugin's existing parameter IDs and automation
path. Change backend policy, pipeline lead and fallback configuration while
processing is stopped, then prepare again and publish the resulting host latency.
Do not route per-block submission or completion servicing through the control
broker, CLI or MCP.

The existing `dev.pulp.gpu/health.read@1` control describes rendering health;
it does not report GPU-audio delivery. Use the audio capability report,
delivery counters and per-block traces for audio diagnostics. A remote audio
report would need its own registered operation and a non-realtime publisher
with a stable producer lifetime. No such broker operation is supplied by these
C++ reports. Include the counter's unit when presenting product-specific
diagnostics: host callbacks and internal processing quantums are not
interchangeable.

A GPU-audio health read operation for the control protocol and MCP is planned
but has not landed. Until it does, read `GpuAudioTransport::status_snapshot()`
from the host.

### Interpret worker timing separately

`Stats::last_block_us` and `avg_block_us` retain their existing names, but their
measurement scope depends on the path. Staged processing times one worker-side
`process_block()` call, including any blocking readback. Shared processing times
a service call that reports progress, which can cover multiple completions.
The average is an exponentially weighted average of those recorded intervals.

These values do not measure GPU execution time, process CPU consumption, or the
callback's deadline margin. Do not turn them into a realtime percentage or
compare them across paths as equivalent per-block costs. Use callback timing,
process CPU measurements, and scope-matched GPU timestamps for those questions.
Likewise, `produced_blocks` records worker progress, not selected GPU output;
use `delivery_snapshot()` for output selection.
