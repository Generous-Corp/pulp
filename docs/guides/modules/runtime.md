# Runtime Module

The runtime module provides lock-free primitives, logging, assertions, and scope guards. It is the lowest-level subsystem — everything else depends on it (via `platform`).

**Status**: stable
**Dependencies**: platform
**Headers**: `pulp/runtime/runtime.hpp` (includes all)

## When to Use Which Primitive

| Pattern | Primitive | Example |
|---------|-----------|---------|
| Single value, latest-wins | `std::atomic<T>` | Parameter values, flags |
| Multi-field coherent read | `SeqLock<T>` | Transport state (tempo + beat position + time sig) |
| Large data swap | `TripleBuffer<T>` | Wavetables, IR buffers, meter data |
| Ordered event stream | `SpscQueue<T>` | MIDI events, UI commands |
| Occurrence-only feedback | `ActivityChannel<N>` | MIDI pad flashes, clip lights |
| Budgeted optional work | `evaluate_runtime_budget()` | Background analysis, voice/cache refresh, validation helpers |

**Never use on the audio thread**: `std::mutex`, `std::condition_variable`, heap allocation, I/O.

## SeqLock

Lock-free reader/writer for coherent multi-field snapshots. The writer increments a sequence counter (odd = writing, even = complete). The reader retries if the sequence was odd or changed during the read.

```cpp
#include <pulp/runtime/seqlock.hpp>

struct TransportState {
    double tempo_bpm;
    double position_beats;
    int time_sig_num;
    int time_sig_denom;
};

pulp::runtime::SeqLock<TransportState> transport;

// Writer (host thread):
transport.write({120.0, 4.0, 4, 4});

// Reader (audio thread — lock-free, never blocks):
auto state = transport.read();
// Guaranteed: all four fields are from the same write
```

Use SeqLock when you need to read multiple related fields as a consistent snapshot. If you only need a single float, use `std::atomic<float>` instead.

### When NOT to use SeqLock

- Single values: use `std::atomic<T>`
- Large data (>cache line): use `TripleBuffer<T>` — SeqLock spins on contention, which is fine for small structs but wasteful for large ones

## TripleBuffer

Lock-free latest-value publication. Three buffers rotate: writer publishes to the back, atomically swaps back↔middle. Reader swaps middle↔front if newer, reads front. Neither side ever blocks.

```cpp
#include <pulp/runtime/triple_buffer.hpp>

struct MeterData {
    float peak_left;
    float peak_right;
    float rms_left;
    float rms_right;
};

pulp::runtime::TripleBuffer<MeterData> meters;

// Audio thread (writer):
meters.write({0.8f, 0.7f, 0.5f, 0.4f});

// UI thread (reader):
const auto& data = meters.read();
update_meter_display(data);
```

Use TripleBuffer for:
- Audio → UI meter data
- Main thread → audio thread config swaps (wavetables, IR buffers)
- Any case where you only care about the latest value, not every value

### TripleBuffer vs SpscQueue

- **TripleBuffer**: reader gets the latest value, intermediate values are discarded. No overflow.
- **SpscQueue**: reader gets every value in order. Can overflow if reader is slow.

## SpscQueue

Single-producer, single-consumer lock-free FIFO. Uses CHOC's `SingleReaderSingleWriterFIFO` under the hood.

```cpp
#include <pulp/runtime/spsc_queue.hpp>

pulp::runtime::SpscQueue<MidiEvent, 256> midi_queue;

// Producer (audio thread):
if (!midi_queue.try_push(event)) {
    // Queue full; overflow_count() / telemetry() make this visible.
}

// Consumer (UI thread):
while (auto ev = midi_queue.try_pop()) {
    handle_event(*ev);
}
```

Use SpscQueue for ordered event streams where every event matters (MIDI,
parameter automation points, UI commands). If the consumer falls behind,
producer-side failed pushes increment `overflow_count()` and appear in
`telemetry()`.

## ActivityChannel

Lock-free occurrence counters for realtime-to-UI feedback where the UI needs
to know that something happened, but does not need an ordered payload for every
event. Multiple signals between UI ticks coalesce into one observation.

```cpp
#include <pulp/runtime/activity_channel.hpp>

enum Voice : std::size_t { Kick, Snare, Hat, VoiceCount };
auto activity = pulp::runtime::make_activity_channel<VoiceCount>();

// Audio thread — lock-free, allocation-free:
activity->signal(Kick);

// UI thread — retain one cursor per lane:
pulp::runtime::ActivityChannel<VoiceCount>::Sequence kick_cursor = 0;
if (activity->consume(Kick, kick_cursor)) {
    flash_kick_pad();
}
```

Give the Processor and editor shared ownership of the channel. That keeps a
host-retained editor safe if the host destroys the Processor first. Use an
`SpscQueue` instead when every event and its payload must be delivered; use a
`TripleBuffer` for latest-value meters.

## Budget Policy

`budget_policy.hpp` provides a small, deterministic decision helper for
runtime work that may need to degrade under load. Critical audio work always
runs. Interactive work can defer to preserve an audio reserve. Background and
opportunistic work can bypass or shed when budget is exhausted or overload is
active.

```cpp
#include <pulp/runtime/budget_policy.hpp>

RuntimeBudgetPolicy policy;
policy.critical_audio_reserve = 128;

RuntimeBudgetRequest request;
request.lane = RuntimeWorkLane::Opportunistic;
request.estimated_cost = 64;
request.remaining_budget = 32;

auto decision = evaluate_runtime_budget(request, policy);
if (!decision.should_run()) {
    // Upload telemetry or use a cheaper fallback; do not block process().
}
```

For a group of optional tasks inside one callback or worker tick, use
`RuntimeBudgetFrame`. It keeps the remaining budget and counts run/defer/shed/
bypass decisions without allocating:

```cpp
RuntimeBudgetFrame frame(512, policy, overload_active);

if (frame.evaluate(RuntimeWorkLane::Interactive, 64).should_run()) {
    refresh_visible_analysis();
}

if (frame.evaluate(RuntimeWorkLane::Opportunistic, 256).should_run()) {
    rebuild_noncritical_preview();
}

auto stats = frame.stats(); // publish off the audio thread if needed
```

This is the intended hook for large graph or instrument consumers: critical
audio remains on the prepared path, while optional analysis, preview, and
background refresh work degrades through the shared policy.

Cost values passed to `RuntimeBudgetFrame` should be stable work units, not
wall-clock timing. Current graph and instrument consumers use deterministic
shape counters: graph node/edge/port/block/buffer stats, or voice-pool
polyphony/active/releasing counts. That makes CI fixtures portable while still
pinning whether optional work runs, defers, sheds, or bypasses at scale.

### Degraded Optional Work Contract

The budget policy is a cooperative decision helper. It does not preempt running
audio work, measure CPU cycles, move work to another thread, or automatically
virtualize graph nodes or instrument voices. Callers must ask before starting
optional work and honor the returned action:

| Action | Caller behavior |
|--------|-----------------|
| `Run` | Execute the optional work inside the caller's prepared budget. |
| `Defer` | Keep required or interactive work queued for a later frame/tick. |
| `Shed` | Drop optional work for this frame/tick without producing stale output. |
| `Bypass` | Use an existing cached/cheap fallback and skip the expensive path. |

Critical audio rendering should not be gated through this helper. Use it for
noncritical graph previews, diagnostics, per-voice analysis, background refresh,
and validation helpers where a degraded result is acceptable and visible.

## ScopeGuard

RAII cleanup — runs a callable on scope exit.

```cpp
#include <pulp/runtime/scope_guard.hpp>

auto guard = pulp::runtime::make_scope_guard([&] {
    cleanup_resource();
});
// cleanup_resource() runs when guard goes out of scope
```

## Logging

```cpp
#include <pulp/runtime/log.hpp>

pulp::runtime::log_info("Plugin loaded: {}", name);
pulp::runtime::log_warn("Buffer underrun at sample {}", position);
pulp::runtime::log_error("Failed to open device: {}", device_name);
```

Logging is safe to call from any thread. Output goes to stderr.

## Assertions

```cpp
#include <pulp/runtime/assert.hpp>

PULP_ASSERT(buffer.num_channels() > 0, "Buffer must have channels");
PULP_DBG_ASSERT(sample_rate > 0, "Sample rate must be positive");
```

`PULP_ASSERT` is active in all builds. `PULP_DBG_ASSERT` is active in
debug builds and compiles away in release builds.
