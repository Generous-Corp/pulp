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
   `state::ParamValue` / `state::StateStore` are lock-free.
3. **Don't block on the main thread.** No `std::cout`, no file I/O,
   no `std::async`, no `dispatch_to_main()`-and-wait. If the audio
   thread sends work to the main thread, it does so via a non-blocking
   queue.

`pulp::runtime::ScopedNoAlloc` (debug builds) tracks rule #1 — Pulp
wraps `View::paint_all` and every adapter's call to
`Processor::process()` in one, so opt-in debug-allocator hooks can
shout when rule #1 is violated. Tooling can read
`pulp::runtime::is_in_no_alloc_scope()` to detect the protected region.

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
accepted/rejected events, and visits nodes in plan order. Snapshot
publication/lifetime policy, audio-buffer routing, feedback delay storage, and
`SignalGraph` mutation belong to later graph-runtime migration slices.

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
or clear/overwrite the requested span. Looping, streaming, interpolation policy,
modulation, and SIMD voice summing remain separate slices.

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
validation polling.

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
needs to observe it.

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

When a background job prepares a new immutable resource for the audio thread,
publish it through `runtime::RealtimeResourceSlot<T, N>`. The control thread
owns `publish()` and `reclaim_retired()`. The audio thread only calls `get()`,
which is a single acquire-load of the latest prepared pointer and does not
allocate, lock, wait, or take ownership. Drain retired resources from the
control side before the fixed reclaim queue fills; if it fills, publication
fails or defers deletion rather than deleting memory a callback may still read.

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

## See also

* [`core/state/include/pulp/state/store.hpp`](../../core/state/include/pulp/state/store.hpp)
  — `snapshot()`, `snapshot_modulated()`, `set_value_rt()`,
  `pump_listeners()`.
* [`core/audio/include/pulp/audio/load_measurer.hpp`](../../core/audio/include/pulp/audio/load_measurer.hpp)
  — load, peak-load, and overload-count snapshots.
* [`core/runtime/include/pulp/runtime/scoped_no_alloc.hpp`](../../core/runtime/include/pulp/runtime/scoped_no_alloc.hpp)
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
