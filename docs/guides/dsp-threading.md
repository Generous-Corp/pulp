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

## See also

* [`core/state/include/pulp/state/store.hpp`](../../core/state/include/pulp/state/store.hpp)
  — `snapshot()`, `snapshot_modulated()`, `set_value_rt()`,
  `pump_listeners()`.
* [`core/runtime/include/pulp/runtime/scoped_no_alloc.hpp`](../../core/runtime/include/pulp/runtime/scoped_no_alloc.hpp)
  — the no-allocation contract.
* sudara, *"Big List of JUCE Tips and Tricks"* #28 (paint = audio)
  and #29 (don't deref atomics per sample).
