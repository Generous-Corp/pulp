# DSP threading — the audio-thread contract

Pulp's audio thread runs at hard real-time priority. Anything that
blocks it — a lock, an allocation, a system call — causes the
listener to hear a dropout. This page covers the small set of rules
that keep your `Processor::process()` callback safe.

If you're coming from JUCE: the rules are almost identical (and the
gotchas are the same). Pulp's API just makes the safe pattern the
default.

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
shout when rule #1 is violated. Future tooling can read
`pulp::runtime::is_in_no_alloc_scope()` to flag a violation.

## Read parameters once per block, not per sample

`store.get_value(id)` is a `std::atomic<float>::load(relaxed)` —
about 1 ns on Apple silicon and similar on x86. Cheap, but not free:
every load is a memory fence that the CPU has to round-trip
through the cache hierarchy, and the compiler can't hoist it out of
your inner loop.

The right pattern is to **snapshot** the parameters you need at the
top of `process()`, then read from the snapshot inside the per-sample
loop:

```cpp
// ❌ Don't: re-read atomic per sample. Each call is a fence.
for (int s = 0; s < n; ++s) {
    out[s] = in[s] * store.get_value(kGainId);
}

// ✅ Do: snapshot once, read locals.
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
    const auto p = state_store().snapshot(kIds);
    const float gain   = p[0];
    const float mix    = p[1];
    const float cutoff = p[2];

    for (int ch = 0; ch < out.num_channels; ++ch) {
        for (int s = 0; s < out.num_frames; ++s) {
            // ... per-sample DSP using locals ...
        }
    }
}
```

For modulated reads (the CLAP per-voice modulation path), use
`snapshot_modulated()` — same idea, but each slot is
`base + mod_offset` instead of just `base`.

## How parameter changes reach the UI

Format adapters now write host-driven parameter changes via
`store.set_value_rt()` (CLAP / VST3 / AU / LV2 — see
[planning/2026-05-18-rt-safety-and-debug-dx.md](../../planning/2026-05-18-rt-safety-and-debug-dx.md)
Slice 2). The RT path is wait-free + alloc-free: it stores the
atomic, then pushes a small `(id, value)` event on a bounded SPSC
queue.

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
(smooth ramps, automation), snapshot once at the top of the block
and interpolate. Pulp doesn't ship a smoother — most DSP that needs
one rolls a tiny `LinearSmoother { current, target, step }`
manually. Future helper TBD.

## Block-scoped runtime contracts

`pulp::format::ProcessBlock` is the additive runtime contract for
new graph, offline-render, sampler, and generated-audio paths. It does
not replace `Processor::process()` yet. It packages the existing
`ProcessContext` with fixed-capacity `BusBufferSet`, non-owning
`EventBlock`, caller-owned `BlockScratch`, explicit `ProcessMode`,
render speed, and reset/bypass/tail/transport-jump flags.

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
and an EventBlock with an empty parameter queue. It does not change the
Processor vtable.

`pulp::audio::SampleZoneMap` is the prepared sampler mapping primitive for
key zones, velocity zones, fixed-pitch triggers, chromatic keytracking,
round-robin groups, slices, and loop metadata. Build or rebuild maps off the
audio thread, then publish immutable snapshots to realtime code. The hot
`ZoneSelector` path is allocation-free and owns no sample storage; zones only
reference `PublishedSampleView` metadata from the publication layer.

`pulp::audio::SamplePool` is the prepared lookup layer for instruments that
need more than one published sample. It maps stable sample IDs to borrowed
`PublishedSampleStore` views and resolves channel pointers without allocation.
Build the pool off the audio thread, keep the borrowed stores alive while
realtime code can read from it, and publish whole-pool snapshots rather than
mutating an active pool in place.

## See also

* [`core/state/include/pulp/state/store.hpp`](../../core/state/include/pulp/state/store.hpp)
  — `snapshot()`, `snapshot_modulated()`, `set_value_rt()`,
  `pump_listeners()`.
* [`core/runtime/include/pulp/runtime/scoped_no_alloc.hpp`](../../core/runtime/include/pulp/runtime/scoped_no_alloc.hpp)
* [`core/format/include/pulp/format/process_block.hpp`](../../core/format/include/pulp/format/process_block.hpp)
* [`core/audio/include/pulp/audio/sample_pool.hpp`](../../core/audio/include/pulp/audio/sample_pool.hpp)
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
