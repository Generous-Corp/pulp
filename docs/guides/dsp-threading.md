# DSP threading — the audio-thread contract

Pulp's audio thread runs at hard real-time priority. Anything that
blocks it — a lock, an allocation, a system call — causes the
listener to hear a dropout. This page covers the small set of rules
that keep your `Processor::process()` callback safe.

Pulp's API is shaped so the safe pattern is the default: allocate and prepare
off the audio thread, then keep `process()` bounded and predictable.

## The three rules

1. **Don't allocate.** No `new`, no `std::vector::push_back`, no
   `std::string` construction. Allocate everything up-front in
   `Processor::prepare()`.
2. **Don't lock.** No `std::mutex::lock()`, no `std::condition_variable`,
   no spinlock with unbounded wait. The atomic accessors on Pulp's
   `state::ParamValue` / `state::StateStore` are lock-free — and
   `state::ParamValue` `static_assert`s `std::atomic<float>::is_always_lock_free`,
   so a target where that fallback would take a hidden mutex fails the build
   rather than silently violating this rule.
3. **Don't block on the main thread.** No `std::cout`, no file I/O,
   no `std::async`, no `dispatch_to_main()`-and-wait. If the audio
   thread sends work to the main thread, it does so via a non-blocking
   queue.

`pulp::runtime::ScopedNoAlloc` tracks rule #1 — Pulp wraps
`View::paint_all` and every adapter's call to `Processor::process()` in
one, and tooling reads `pulp::runtime::is_in_no_alloc_scope()` to detect
the protected region. The production guard is a no-op under `NDEBUG`
(zero release cost); the *enforcement* lives in the test harness (see
"Verifying the contract in tests" below), where an allocation or a
blocking lock inside the scope aborts the binary.

## Numeric mode: denormals & determinism

Denormal (subnormal) floats stall the audio thread when they enter
recursive state — IIR/SVF filter memory, reverb/delay feedback, envelope
tails decaying toward silence. Pulp addresses this at two levels:

1. **Per-value (opt-in):** `pulp::signal::snap_to_zero` (denormal.hpp)
   snaps a value or buffer below ~-300 dB FS to exactly zero. Use it at
   the end of each recursive state update. It is the portable baseline
   and works regardless of CPU mode.
2. **Callback-scope (hardware):** `pulp::signal::ScopedFlushDenormals`
   (scoped_flush_denormals.hpp) sets the CPU flush-to-zero mode — MXCSR
   FTZ on x86-64, FPCR.FZ on AArch64 — for the lifetime of the
   scope, then restores the caller's previous mode. It protects *all*
   DSP in the callback, including code that forgot rule-1's
   `snap_to_zero`. Wrap the callback body alongside `ScopedNoAlloc`:

   ```cpp
   pulp::signal::ScopedFlushDenormals flush_denormals;
   processor.process(...);
   ```

   On targets without a hardware mode (e.g. MSVC/ARM64) the guard is a
   correct no-op (`kHardwareFlushSupported == false`); `snap_to_zero`
   remains the safety net there. The standalone host and **every** format
   adapter — VST3, AU v2, AU v3, CLAP, and AAX — wrap their real-time
   callback body in this guard, so a plugin gets denormal flushing in every
   host, not just the standalone app.

**Determinism contract.** Flush-to-zero changes denormal results to
zero, so a flushed render is *not bit-identical* to an unflushed one for
signals that decay into the denormal range — but it is the audibly
correct, stall-free result, and it is deterministic given a fixed
numeric mode. Offline/golden renders therefore fix the numeric mode they
assert against. A future parallel graph mode that reorders summation
will be documented as *sample-equivalent*, not bit-identical, unless it
pins a deterministic reduction order.

## Budget prepare-time resources

`format::PrepareContext` carries optional `resource_limits` for hosts and test
harnesses that need fail-closed behavior before large allocations happen. A zero
limit means "unspecified/unlimited" for source compatibility.

Processors with large prepared storage can override
`Processor::estimate_prepare_resources()` to report persistent bytes, fixed
per-block scratch bytes, block size, channel counts, event capacities, and voice
capacity. Hosts can call `Processor::check_prepare_resource_limits()` before
`prepare()` and reject configurations that exceed a non-zero limit; the
headless test/batch host exposes this as `HeadlessHost::try_prepare()`. This
keeps oversized samplers, convolution IRs, analysis caches, and voice pools from
discovering budget failure on the audio thread.

A failed `HeadlessHost::try_prepare()` is a preflight failure, not a partial
reconfiguration. It does not call `Processor::prepare()` and does not replace
the last successful prepared render context, so batch tools can probe tighter
budgets without invalidating the processor that is already prepared.

Memory-pressure callbacks run on an owner thread and are allowed to drop
rebuildable caches so a later prepare retry fits tighter limits. They must keep
the prepared core state coherent, keep fixed per-block scratch accounted in
`estimate_prepare_resources()`, and never perform audio-thread recovery work.

## Read parameters once per block, not per sample

`store.get_value(id)` is a `std::atomic<float>::load(relaxed)`. Cheap,
but not free: repeated atomic loads add inner-loop work and prevent the
compiler from treating the value as an ordinary loop-local scalar.

The right pattern is to **snapshot** the parameters you need at the
top of `process()`, then read from the snapshot inside the per-sample
loop:

```cpp
// Don't: re-read the atomic value per sample.
for (int s = 0; s < n; ++s) {
    out[s] = in[s] * store.get_value(kGainId);
}

// Do: snapshot once, read locals.
const float gain = store.get_value(kGainId);
for (int s = 0; s < n; ++s) {
    out[s] = in[s] * gain;
}
```

For multiple parameters, Pulp's `StateStore::snapshot()` returns a
stack-allocated `std::array<float, N>` so you can grab them all in
one go:

```cpp
constexpr std::array<state::ParamID, 3> kIds = {
    kGainId, kMixId, kCutoffId,
};

void MyPlugin::process(audio::BufferView<float>& out,
                       const audio::BufferView<const float>& in,
                       midi::MidiBuffer&, midi::MidiBuffer&,
                       const ProcessContext&) {
    const auto p = state().snapshot(kIds);
    const float gain   = p[0];
    const float mix    = p[1];
    const float cutoff = p[2];

    for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
        for (std::size_t s = 0; s < out.num_samples(); ++s) {
            // ... per-sample DSP using locals ...
        }
    }
}
```

For modulated reads (the CLAP per-voice modulation path), use
`snapshot_modulated()` — same idea, but each slot is
`base + mod_offset` instead of just `base`.

## How parameter changes reach the UI

Format adapters write host-driven parameter changes via
`store.set_value_rt()` (CLAP / VST3 / AU / LV2). The RT path is
wait-free + alloc-free: it stores the atomic, then pushes a small
`(id, value)` event on a bounded SPSC queue.

The UI thread drains the queue by calling `store.pump_listeners()`
each frame; that's where `ListenerThread::Main` listeners actually
fire. So:

* **Audio thread:** atomic write + lock-free queue push, no `new`,
  no `EventLoop::dispatch` lambda.
* **UI thread:** drain the queue, fan out to listeners.

You don't usually call `pump_listeners()` yourself — `pulp::view`'s
editor tick does it. But it's the right hook if you embed the
StateStore in a non-Pulp host.

## Block-level vs sample-level changes

If you want the parameter change to take effect *within* a block
(automation splits, smooth ramps), use the parameter-event helpers
instead of polling atomics inside the loop:

* `param_events()` exposes the host-delivered sparse
  `ParameterEventQueue` for the current block.
* `ParameterEventQueue` is fixed-capacity; excess events are dropped,
  counted for the current block, and never trigger audio-thread allocation.
* `format::EventBlock::audio_rate_modulations` carries dense
  per-sample audio-rate parameter lanes for ProcessBlock-native runtimes.
  These lanes are separate from `ParameterEventQueue`; do not expand a
  dense lane into one event per sample.
* `format::ParamCursor` advances parameter values at sample offsets and
  interpolates active `ramp_duration_sample_frames` events.
* `format::for_each_subblock()` slices the audio block at parameter
  event boundaries so your DSP can render each stable span.
* `format::ControlRateParamSmoother` follows the parameter's configured
  `smoothing_ramp_seconds` for control-rate smoothing.

MIDI block buffers follow the same bounded-storage rule. Adapter-owned
`midi::MidiBuffer` and `midi::UmpBuffer` instances must reserve their
worst-case event/SysEx capacities before the callback and enable
`set_realtime_capacity_limit(true)` if they append on the audio thread.
Prepared buffers drop and count overflow instead of growing vectors.
Unprepared buffer mutation, SysEx payload allocation, MIDI file editing, and
sequence-building helpers belong on control/offline threads. The UI-to-audio
`midi::MidiMessageCollector` uses a fixed-capacity SPSC queue; its audio-thread
`drain_into()` path is allocation-free when the destination `MidiBuffer` has
prepared capacity.

Signal helper utilities use the same distinction. Stateless math, interpolation,
denormal, fixed-size matrix, color/frequency mapping, and scalar coefficient
calculation helpers are allocation-free. Helpers that create `std::vector`
results, resize aligned buffers, generate FFT windows, design high-order filter
cascades, configure spectrogram storage, or build FFT/convolution state are
prepare/control/offline work. After that setup, documented hot operations such
as precomputed window application, configured spectrogram column pushes, and
aligned-buffer clear/copy are allocation-free.

## Block-scoped runtime contracts

`pulp::format::ProcessBlock` is the additive runtime contract for
new graph, offline-render, sampler, and generated-audio paths. It does
not replace `Processor::process()` yet. It packages the existing
`ProcessContext` with fixed-capacity `BusBufferSet`, non-owning
`EventBlock`, caller-owned `BlockScratch`, explicit `ProcessMode`,
render speed, and reset/bypass/tail/transport-jump flags. `EventBlock`
keeps sparse `ParameterEventQueue` automation and dense
`AudioRateModulationView` lanes as separate borrowed views.

`ProcessContext::process_mode` tells a processor whether the current block is
live realtime audio or an offline render. Existing adapters default to
`ProcessMode::Realtime`; headless and export-style hosts should set
`ProcessMode::Offline` explicitly when they drive deterministic non-live
processing. Use the helper predicates instead of comparing raw enum values in
hot code. A realtime block with a slower-than-realtime hint is still an
audio-thread callback. Bypass, tail-drain, reset, and transport-jump flags are
block metadata for processors that need to distinguish those host states
without inferring them from transport fields. `HeadlessHost::process(...,
ProcessContext)` forwards those flags unchanged, so tests can cover
runtime-mode decisions without a plug-in format SDK.

The contract is deliberately non-owning: bus audio is borrowed from the
host or renderer, event containers are owned by adapters, and scratch
memory is provided before the callback. This keeps the same hard
real-time rule as `Processor::process()`: no hidden allocation, locks,
file I/O, logging, package work, or host calls in the audio callback.

`pulp::format::OfflineRenderHost` is the control-thread companion for
multi-block renders, bounces, freeze-to-sample tests, and generated-audio
materialization. It owns result/staging buffers and may allocate outside the
audio callback, but each processor invocation is still a bounded
`HeadlessHost` block with explicit `ProcessContext`, MIDI, and parameter-event
slices. Use `HeadlessHost` when a test needs one block; use
`OfflineRenderHost` when it needs a fixed-duration render with final-block
handling, silence padding, and absolute-frame event slicing.

`pulp::graph::GraphRuntimeQueues` is the fixed-capacity control/realtime
handoff for graph-runtime work. Control code enqueues graph commands; the
realtime graph drains and sorts them by block offset at the start of a block.
The realtime graph publishes bounded control events and MIDI output events
through separate RT-to-control queues with explicit drop counters. The queue
primitive does not mutate `SignalGraph` directly; graph v2 and adapter
migration code consume these commands against their active snapshot policy.

`pulp::graph::GraphRuntimePlan` is the control-thread topology compiler for
graph v2 work. It turns sparse node IDs into dense node/connection arrays,
precomputes inbound/outbound connection ranges, rejects over-limit graphs,
validates ports, and refuses cycles unless a connection is explicitly marked as
feedback. Build or rebuild plans off the audio thread, then publish immutable
snapshots to realtime code. The old `pulp::host` graph-runtime headers remain
compatibility aliases, but the platform-neutral `pulp::graph` module is the
canonical owner.

`pulp::format::GraphRuntimeSnapshot` and `GraphRuntimeExecutor` are the first
`ProcessBlock`-based graph execution seam. Build immutable snapshots off the
audio thread by binding one process callback per dense graph node, then pass the
active snapshot into each executor call. Do not reset or clear a snapshot while
any realtime call may still reference it. The executor validates the block,
drains graph queues without heap allocation, emits bounded command
accepted/rejected events, and visits nodes in plan order.

`GraphRuntimeExecutor::process_routed()` adds inter-node audio routing on top of
that walk for routed graphs. The snapshot computes a
`graph::GraphRuntimeBufferAssignment` off the audio thread (one mono scratch
slot per node input/output port); the caller pre-allocates a
`GraphRuntimeBufferPool` of that many slots and reuses it across blocks. Per
node the executor gathers (zero then sum) each non-feedback inbound connection
from its upstream output slot, then either copies the main I/O bus (AudioInput /
AudioOutput nodes) or hands the binding `node_inputs` / `node_outputs` views over
its scratch slots. The routed path allocates and locks nothing on the RT thread
(the pool must already `fits()` the snapshot for the block) and matches
`host::SignalGraph` bit-for-bit. Feedback connections are honored as a one-block
delay: the assignment appends one persistent previous-block slot per feedback
edge, gather adds that slot (last block's captured source output), and after the
walk the source's output is captured into it — mirroring SignalGraph's
`feedback_prev`, with the pool's zero-init giving the first block silent
feedback. Per-connection PDC delay lines, sidechain inputs, MIDI edges, sparse
automation, dense audio-rate modulation, slot reuse, and the `SignalGraph`
process migration are part of the current routed path; the executor fails closed
when a graph exceeds the fixed per-node automation scratch capacity.

`host::SignalGraph` can request routing its live `process()` callback through
this executor for eligible graphs via
`set_canonical_executor_routing_enabled(true)` — default OFF, with the legacy
walk as the fallback for ineligible graphs and the parity oracle. Eligible nodes
are AudioInput, AudioOutput, Gain, live Plugin, MidiInput, and MidiOutput nodes;
eligible edges are feedforward/feedback/sidechain audio, MIDI, sparse
automation, and dense audio-rate modulation. The routing snapshot and its
scratch pool are built in `compile_()` and embedded per-snapshot in the
published `CompiledGraph`, so a re-prepare builds fresh ones on a fresh snapshot
and never resizes a buffer an in-flight audio reader is using — the same
RCU/retire discipline the legacy per-node scratch relies on.

`SignalGraph::set_parallel_routing_enabled(true)` requests the levelized
parallel executor for the same eligible subset. The parallel-safe snapshot uses
reuse-free scratch slots, levels containing `AudioOutput` still run serially to
preserve accumulation order, and `set_parallel_min_work_units()` can keep
low-cost levels serial when their static work-weight x block size is below the
fork/join break-even threshold. `routing_executor_stats()` is the diagnostic
surface for tests and tools that need to prove which routed path ran.

Parallel renderers opt into native scheduling through
`format::AudioWorkgroupClient`, a capability kept separate from `Processor`'s
node ABI. Standalone forwards `AudioDevice::callback_workgroup()` after the
device opens and removes it before close; live default-device changes stop the
callback, publish null, wait for every worker to acknowledge leaving, then
re-query and publish the replacement before rendering resumes. Standalone uses
the same off-render-thread drain barrier before closing the device, because the
handle is borrowed from the open device. Before that final drain, the device
disables change notifications and serializes with any default-device switch
already in flight; if the switch rebound a replacement handle, the serialized
close path publishes null and drains it too. AUv3 forwards host context changes
through `renderContextObserver`. `GraphRuntimeWorkerPool` generation-publishes
each change so workers leave and join on their own threads. Until every worker
has adopted the current generation, the next batch runs inline on the render
thread rather than dispatching into a stale workgroup. A failed non-null join
does not acknowledge the generation: that worker retries and batches remain
inline until adoption succeeds or the owner publishes removal.

`SignalGraph::set_anticipation_enabled(true)` adds an opt-in anticipative splice
on top of the canonical routed path. When `prepare()` finds an eligible latent
interior, the graph builds an `AnticipationLane`; a single background producer
calls `pump_anticipation()` to pre-render that interior ahead of the deadline,
and `process()` consumes one pre-rendered boundary block, pre-fills the routed
executor slots, and masks the interior so its plugin state is not advanced twice.
Hosts must stop and join the producer before `prepare()` or graph mutation, and
underruns produce silence for the spliced boundary instead of falling back to a
live interior render.

`pulp::format::process_processor_block()` is the additive bridge from
`ProcessBlock` back to the legacy `Processor::process()` ABI. It requires an
active main output bus, allows output-only instrument blocks, publishes
EventBlock sidecars for the duration of the call, restores any previous
Processor sidecar pointers, and preserves the distinction between no EventBlock
and an EventBlock with an empty parameter queue. Dense audio-rate modulation
lanes are not collapsed into legacy `Processor::param_events()`. It does not
change the Processor vtable.

`pulp::audio::rt_safety_contract.hpp` is the machine-checkable sampler/looper
RT-safety label table. It classifies representative public DSP helpers as
audio-callback safe, safe only after `prepare()`, safe only with immutable or
generation-pinned inputs, telemetry-only, control-thread only, background-thread
only, or offline-only. Treat the table as a callback boundary contract: anything
marked audio-callback allowed must not allocate, lock, block, call hosts, do file
I/O, or run package analysis. Import/export, waveform thumbnails, onset/slice
analysis, loop search, publication writes, and materialization stay outside the
audio callback even when their low-level copying helpers are allocation-free.
When adding sampler/looper DSP helpers, add or update a contract row and prefer
conservative `may_allocate`, `may_lock`, and `may_block` flags unless the
implementation and its callees have been audited.

The same header's `core_runtime_rt_safety_contracts()` is the sibling registry
for the core RT runtime — the lock-free primitives (`SeqLock`, `TripleBuffer`,
`SpscQueue`), `ParameterEventQueue`, `AudioProcessLoadMeasurer`,
`ScopedFlushDenormals`, `SignalGraph::process`, and `Processor::process`. Both
tables are drift-checked against the same well-formedness invariants
(audio-callback-allowed ⇒ no alloc/lock/block; lock ⇒ block; unique rows) in
`test_sampler_rt_safety_contract.cpp` / `test_core_runtime_rt_safety_contract.cpp`.
The labels are descriptive; the *enforcement* is the no-alloc/no-lock abort-trap
(see "Verifying the contract in tests").

`pulp::audio::SampleZoneMap` is the prepared sampler mapping primitive for
key zones, velocity zones, fixed-pitch triggers, chromatic keytracking,
round-robin groups, slices, and loop metadata. Build or rebuild maps off the
audio thread, then publish immutable snapshots to realtime code. The hot
`ZoneSelector` path is allocation-free and owns no sample storage. Zones may
carry direct `PublishedSampleView` metadata for simple selection paths, or a
stable sample ID for pool-backed instrument runtime paths.

`pulp::audio::SamplePool` is the prepared lookup layer for instruments that
need more than one published sample. It maps stable sample IDs to borrowed
`PublishedSampleStore` views and resolves channel pointers without allocation.
Build the pool off the audio thread, keep the borrowed stores alive while
realtime code can read from it, and publish whole-pool snapshots rather than
mutating an active pool in place.

`pulp::audio::InstrumentRuntime` is currently the RT trigger-resolution join
between `SampleZoneMap` and `SamplePool`: it chooses a pool-backed zone,
resolves the zone's stable sample ID, and computes the playback rate from pitch
policy plus the resolved sample rate. It intentionally does not allocate
voices, enforce choke groups, stream sample tails, apply modulation, or render
audio.

`pulp::audio::InstrumentVoiceAllocator` is the prepared voice-slot policy layer
for trigger allocation, release, deterministic stealing, voice groups, and
choke groups. Call `prepare()` off the audio thread, then trigger/release
against the fixed voice array in realtime code. Note-off moves a voice to
`Released`; the future envelope/render layer should call `finish_voice()` when
the tail is done. Choke and steal are force-termination events and can be
reported through caller-owned spans. The allocator does not own sample data, run
envelopes, apply modulation, or render voices.

`pulp::audio::AhdsrEnvelope` is the per-voice AHDSR/ADSR gain primitive. Call
`prepare()` off the audio thread, then drive `note_on()`, `note_off()`,
`next_sample()`, or `render()` from realtime voice code. `render()` writes gain
values into caller-owned buffers; it does not multiply audio or own voice
storage.

`pulp::audio::SampleVoiceRenderer` is the first scalar sample-reader voice
primitive. It renders one-shot forward playback from a `SamplePoolResolution`
into a caller-owned `BufferView`, using caller-owned source-channel scratch and
an optional per-voice `AhdsrEnvelope`. It can accumulate into an output buffer
or clear/overwrite the requested span. Its compatible region modes reuse
`LoopReader` tap mapping, but new rich-loop playback should use
`LoopPlaybackCursor`, `PreparedSampleInterpolation`, and `LoopRenderer`.
Streaming, modulation, and SIMD voice summing remain separate concerns.

Representative sampler callback paths are explicitly inventoried:

| Callback-safe after preparation | Off-thread owner work |
|---|---|
| `SampleAssetView` metadata/preload reads | `SampleAsset` prepare/release |
| prepared interpolation footprint/evaluation | cache prepare, fill completion, eviction, and reclamation |
| cursor read planning and advance | decoder worker calls and completion publication |
| demand/cancel inbox pushes | async-service demand draining and retirement |
| cache-service page-read begin/complete | memory-governor reservation and lease release |
| narrow and loop-aware paged-reader plan/render | file decode/import and asset preparation |
| starvation-envelope gain transitions | onset/slice analysis and offline stretch |
| PulpTempoSampler steady-state `process()` | PulpTempoSampler stretch and matched-source publication |

The complete machine-checked rows and ownership notes live in
`rt_safety_contract.hpp`. PulpTempoSampler publishes one resident sample view
and its fixed-capacity slice regions as a matched `TripleBuffer` value; its
callback does not take the control-side raw-analysis mutex.

Prefer the `ProcessContext` predicates for common block-policy decisions:
`should_reset_dsp_state()` covers explicit reset requests and derived transport
jumps, `is_maintenance_render()` covers bypass or tail-drain calls that are not
normal input-driven renders, and `should_render_tail_only()` identifies blocks
where the host wants existing delay/reverb/lookahead state to settle without
starting new work.

## Publish cheap runtime telemetry

Live tools should read bounded snapshots from the audio thread, not run
analysis on it. `audio::AudioProcessLoadMeasurer` publishes relaxed latest-
value telemetry for callback count, elapsed time, buffer budget, current load,
peak load, and overload count. `audio::AudioDeviceManager` combines that
process-load snapshot with its xrun counter for UI, Audio Inspector, and
validation polling. Polling `runtime_telemetry_snapshot()` is expected to stay
allocation-free; it should aggregate already-published counters, not allocate
or query the audio backend.

For hosted graphs, `host::SignalGraph::node_loads()` exposes the same
`AudioProcessLoadSnapshot` *per node*: `process()` wraps each node's work in a
persistent per-node `AudioProcessLoadMeasurer` (relaxed-atomic begin()/end(),
RT-safe), and `node_loads()` returns a latest-value snapshot per node from the
control thread. The measurers persist across `prepare()` recompiles so a node's
load history survives topology changes, and they are only ever added (never
erased) while a snapshot is live, so the audio thread's raw measurer pointers
stay valid across snapshot swaps. This is what lets a host attribute a CPU
spike to a specific node instead of just the whole callback.

Validation and UI surfaces should classify that snapshot with
`audio::evaluate_audio_runtime_overload()` instead of inventing local
thresholds. The shared policy reports nominal, watch, overloaded, and critical
states, and distinguishes optional-work shedding from optional-work bypass and
validation failure. This keeps the audio callback limited to publishing cheap
counters while host-lab reports, agents, and inspector surfaces make consistent
post-callback decisions.

Audio Inspector is a consumer of these snapshots, not the producer. Feed its
runtime telemetry bridge from `AudioDeviceManager::runtime_telemetry_snapshot()`
or another bounded source so agents can inspect load/xrun state through the
inspector protocol without adding locks, allocations, or analysis work to
`process()`.
The non-GPU inspector-domain tests cover this owner-thread bridge explicitly:
manager load/xrun snapshots are copied into `AudioInspector` as latest values
for UI and agent polling.

`state::ParameterEventQueue` also exposes fixed-size queue telemetry, including
its monotonic overflow count, so automation drops are visible instead of only
being implied by a failed `push()`.

`state::StateStore::rt_listener_queue_telemetry()` reports pressure on the
`set_value_rt()` to `pump_listeners()` queue, including skipped Main-listener
notifications when UI polling falls behind.

For ordered audio-thread/UI-thread event streams, `runtime::SpscQueue` exposes
the same producer-side overflow count and telemetry snapshot.

`midi::MidiMessageCollector` combines the producer queue snapshot with its
consumer-owned pending-ring occupancy and future-event drop counter, making
UI-to-audio MIDI back-pressure visible without inspecting the audio callback.

`midi::Synthesiser` and `midi::MpeVoiceAllocator` expose owner-thread voice
telemetry snapshots for polyphony, active/releasing voice counts, and steal
counts. Read those snapshots from the processor/audio owner and publish the
returned value through a lock-free latest-value channel when UI or tooling
needs to observe it. For optional instrument-side work such as per-voice
analysis refresh, preview rendering, or diagnostics, call
`evaluate_optional_runtime_budget()` with a `runtime::RuntimeBudgetFrame`.
The helper uses voice-pool telemetry to produce the shared run/defer/shed/bypass
decision while the actual voice render path stays on the normal prepared audio
path. The voice cost model is deterministic: polyphony capacity, active voices,
and releasing voices. It is meant for portable budget fixtures and optional-work
fallbacks, not CPU timing. The helper does not drop active notes, preempt voice
rendering, or change voice-stealing policy; it only tells optional work whether
to run, defer, shed, or bypass.

Keep this path boring: write fixed-size counters, meter snapshots, and bounded
queues from `process()`. Move FFTs, exported waveforms, parameter sweeps, and
other expensive analysis to an offline command, frozen copy, or validation
artifact.

## Run resource work off the audio thread

Use `runtime::BackgroundJobService` for cancellable non-RT resource work such as
IR loading, sample import, preset restore, waveform analysis, and other jobs
that may allocate, block, report progress, or touch the filesystem. Jobs receive
a `runtime::BackgroundJobContext` with a shared `CancellationToken` and a
progress publisher. Call `BackgroundJobHandle::wait()` only from a control,
test, or teardown thread; never wait from `process()`.
For owner teardown, call `BackgroundJobService::cancel_all()` and then
`wait_all()` from a non-RT thread so running jobs can observe cancellation and
queued jobs can be drained without executing stale work.

When a background job prepares a new immutable resource for the audio thread,
publish it through `runtime::RealtimeResourceSlot<T, N>`. The control thread
owns `publish()` and `reclaim_retired()`. The audio thread only calls `get()`,
which is a single acquire-load of the latest prepared pointer and does not
allocate, lock, wait, or take ownership. Drain retired resources from the
control side before the fixed reclaim queue fills; if it fills, publication
fails or defers deletion rather than deleting memory a callback may still read.
Track `retired_count_approx()` and `retire_overflow_count()` from the control
side when resources can be regenerated faster than teardown drains them.

For optional work that can degrade, use `runtime::evaluate_runtime_budget()` to
make the run/defer/shed/bypass decision explicit. Critical audio work remains
on the prepared path, interactive work can defer to preserve a reserve, and
background/opportunistic analysis can be shed or bypassed during overload
instead of competing with the callback. For a group of optional tasks in one
callback or worker tick, `runtime::RuntimeBudgetFrame` applies the same policy
while tracking remaining budget and degradation counters without allocation.

Common recipes:

* IR load: decode and resample the impulse response in a background job, build
  the immutable convolution resource there, then publish the prepared resource
  to `RealtimeResourceSlot` after checking the job has not been cancelled.
* Preset or resource restore: parse files, validate schema, and resolve missing
  resource diagnostics off the audio thread. Publish only the final immutable
  state snapshot; keep the old snapshot active if validation or cancellation
  wins.
* Waveform or analysis refresh: copy or freeze the input range first, run FFT,
  thumbnail, or statistics work in the job, and publish a bounded UI snapshot
  instead of reading editor data from `process()`.
* Sample import: decode, normalize, build loop metadata, and prefetch pages in
  the job. Publish a prepared sample-map revision atomically; treat cache misses
  as control-thread work, not audio-thread file I/O.

## Verifying the contract in tests

The no-alloc / no-lock contract is *enforced*, not just documented. On
UNIX test builds, `test/native_components/rt_intercept_test_support.cpp`
installs a strong `pulp_rt_trap_if_no_alloc_scope` plus global
`operator new`/`new[]` overrides and `pthread` mutex/rwlock
interposers. Inside a no-alloc scope, any heap allocation or blocking
lock writes `[pulp-rt-trap] allocation inside no-alloc scope` and
`abort()`s — so a violation fails the test by killing the process, not
by a soft assertion. A Rust `#[global_allocator]` routes the checking
core's allocations through the same trap. On non-UNIX builds the
fallback `RtAllocationProbe` *counts* allocations instead.

`test/harness/scoped_rt_process_probe.hpp` exposes the shared
`pulp::test::ScopedRtProcessProbe`, which enters an always-on
`RtNoAllocScope` (independent of `NDEBUG`, so the check is live even in
Release test binaries) together with a `ScopedNoAlloc`. Wrap an RT path
in it and assert `allocation_count() == 0`:

```cpp
pulp::test::ScopedRtProcessProbe probe;
graph.process(out_view, in_view, num_frames);
REQUIRE(probe.allocation_count() == 0);  // trap build also proves lock-freedom
```

To opt a test target into the trap, link
`native_components/rt_intercept_test_support.cpp` on UNIX and define
`PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1`. Current coverage:
`StateStore` RT writes and `NativeCoreProcessor::process`
(`test_rt_safety.cpp`), and the host graph hot path
`SignalGraph::process()` (`test_signal_graph_rt_safety.cpp`).

## See also

* [`core/state/include/pulp/state/store.hpp`](../../core/state/include/pulp/state/store.hpp)
  — `snapshot()`, `snapshot_modulated()`, `set_value_rt()`,
  `pump_listeners()`.
* [`core/audio/include/pulp/audio/load_measurer.hpp`](../../core/audio/include/pulp/audio/load_measurer.hpp)
  — load, peak-load, and overload-count snapshots.
* [`core/runtime/include/pulp/runtime/scoped_no_alloc.hpp`](../../core/runtime/include/pulp/runtime/scoped_no_alloc.hpp)
* [`core/signal/include/pulp/signal/scoped_flush_denormals.hpp`](../../core/signal/include/pulp/signal/scoped_flush_denormals.hpp)
  — hardware flush-to-zero guard for the callback boundary.
* [`core/format/include/pulp/format/process_block.hpp`](../../core/format/include/pulp/format/process_block.hpp)
* [`core/audio/include/pulp/audio/instrument_envelope.hpp`](../../core/audio/include/pulp/audio/instrument_envelope.hpp)
* [`core/audio/include/pulp/audio/instrument_runtime.hpp`](../../core/audio/include/pulp/audio/instrument_runtime.hpp)
* [`core/audio/include/pulp/audio/instrument_voice_allocator.hpp`](../../core/audio/include/pulp/audio/instrument_voice_allocator.hpp)
* [`core/audio/include/pulp/audio/rt_safety_contract.hpp`](../../core/audio/include/pulp/audio/rt_safety_contract.hpp)
  — machine-checkable sampler/looper callback-boundary labels.
* [`core/audio/include/pulp/audio/sample_pool.hpp`](../../core/audio/include/pulp/audio/sample_pool.hpp)
* [`core/audio/include/pulp/audio/sample_voice_renderer.hpp`](../../core/audio/include/pulp/audio/sample_voice_renderer.hpp)
* [`core/audio/include/pulp/audio/sample_zone_map.hpp`](../../core/audio/include/pulp/audio/sample_zone_map.hpp)
  — the no-allocation contract.
* [`core/format/include/pulp/format/offline_render_host.hpp`](../../core/format/include/pulp/format/offline_render_host.hpp)
  — the deterministic multi-block render harness.
* [`core/graph/include/pulp/graph/graph_runtime_queue.hpp`](../../core/graph/include/pulp/graph/graph_runtime_queue.hpp)
  — the graph command/event/MIDI handoff queues.
* [`core/graph/include/pulp/graph/graph_runtime_plan.hpp`](../../core/graph/include/pulp/graph/graph_runtime_plan.hpp)
  — dense graph topology plans and bounded validation.
* [`core/format/include/pulp/format/graph_runtime_executor.hpp`](../../core/format/include/pulp/format/graph_runtime_executor.hpp)
  — `ProcessBlock` graph snapshot/executor seam.
* [`core/format/include/pulp/format/processor_block_adapter.hpp`](../../core/format/include/pulp/format/processor_block_adapter.hpp)
  — `ProcessBlock` to `Processor::process()` bridge.
* sudara, *"Big List of JUCE Tips and Tricks"* #28 (paint = audio)
  and #29 (don't deref atomics per sample).
