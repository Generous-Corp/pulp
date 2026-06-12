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

* `param_events()` exposes the host-delivered `ParameterEventQueue`
  for the current block.
* `format::ParamCursor` advances parameter values at sample offsets.
* `format::for_each_subblock()` slices the audio block at parameter
  event boundaries so your DSP can render each stable span.
* `format::ControlRateParamSmoother` follows the parameter's configured
  `smoothing_ramp_seconds` for control-rate smoothing.

## Realtime vs offline process modes

`ProcessContext::process_mode` tells a processor whether the current block is
live realtime audio or an offline render. Existing adapters default to
`ProcessMode::Realtime`; headless and export-style hosts should set
`ProcessMode::Offline` explicitly when they drive deterministic non-live
processing.

Use the helper predicates instead of comparing raw enum values in hot code:

```cpp
void MyPlugin::process(audio::BufferView<float>& out,
                       const audio::BufferView<const float>& in,
                       midi::MidiBuffer& midi_in,
                       midi::MidiBuffer& midi_out,
                       const ProcessContext& ctx) {
    if (ctx.is_realtime()) {
        // Hard realtime: no allocation, no locks, no blocking work.
    }

    if (ctx.allows_offline_quality_work()) {
        // Offline render: choose a bounded higher-quality path that was
        // prepared ahead of time.
    }
}
```

The mode does not relax buffer ownership or lifetime rules. A realtime block
with a slower-than-realtime hint is still an audio-thread callback. Bypass,
tail-drain, reset, and transport-jump flags are block metadata for processors
that need to distinguish those host states without inferring them from
transport fields.

## See also

* [`core/state/include/pulp/state/store.hpp`](../../core/state/include/pulp/state/store.hpp)
  — `snapshot()`, `snapshot_modulated()`, `set_value_rt()`,
  `pump_listeners()`.
* [`core/runtime/include/pulp/runtime/scoped_no_alloc.hpp`](../../core/runtime/include/pulp/runtime/scoped_no_alloc.hpp)
  — the no-allocation contract.
