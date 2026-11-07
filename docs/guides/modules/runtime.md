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
| Wake a sleeping worker | `WakeSignal` | Audio thread hands a block to a render or disk thread |
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

## WakeSignal

Wakes one sleeping consumer thread from a realtime producer. The producer
publishes its work first (a ring write, a release store) and then calls
`notify()`, which is a fence and one atomic load: it posts a semaphore only
while the consumer is actually parked, so a busy consumer costs the audio
thread no system call. The consumer waits with a predicate over the same
published state and re-checks it after announcing that it is about to park,
so a wakeup cannot be lost.

```cpp
#include <pulp/runtime/wake_signal.hpp>

pulp::runtime::WakeSignal wake;

// Audio thread:
ring.push(block);
wake.notify();

// Worker thread:
while (running) {
    wake.wait_for([&] { return !ring.empty() || !running; }, std::chrono::seconds(1));
    drain(ring);
}
```

Use it instead of `std::condition_variable::notify_*` from the audio thread,
which is not realtime safe and loses wakeups unless the producer takes the
mutex. A worker that waits on a `WakeSignal` needs no short polling timeout,
so it does not wake the CPU while idle. One consumer per signal; `posts()`,
`wakes()` and `consumer_parked()` are diagnostics for tests.

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

## Cancellation

`CancelSource` owns a cancellation request and `CancelToken` observes it. Hand the
token to the work being cancelled and keep the source with whoever may cancel it.

```cpp
#include <pulp/runtime/cancel.hpp>

pulp::runtime::CancelSource source;
auto token = source.token();                 // copyable observer

worker = std::thread([token] {
    while (!token.is_cancelled()) {          // lock-free; safe on the audio thread
        do_a_slice_of_work();
    }
});

auto registration = token.register_callback([] { wake_the_ui(); });
bool stopped = token.wait_for_cancellation(std::chrono::seconds(5));

source.cancel();                             // idempotent; also runs on destruction
```

- `CancelSource` is move-only and cancels when destroyed, so work never outlives
  its owner without being told to stop.
- A default-constructed `CancelToken` can never be cancelled, which makes
  `const CancelToken& = {}` a safe "not cancellable" default parameter.
- `is_cancelled()` is one acquire load. It never locks or allocates. Everything
  else takes a mutex and stays off the audio thread.
- `register_callback` returns a `CancelRegistration`. Callbacks run on the
  cancelling thread, in registration order, or immediately if the token is
  already cancelled. Destroying a registration waits for a callback that is
  running on another thread, so captured state is safe to free afterwards.
- `CancellationToken` is the former single type that both requested and observed
  cancellation. It still works and converts to `CancelToken`.

## Special directories

`pulp::platform::special_dir` (in `pulp/platform/special_dir.hpp`, which this module
depends on) is the one place that knows where each OS keeps the user's folders.
Settings, presets, content and log code call it instead of carrying their own
`#ifdef` ladders.

```cpp
#include <pulp/platform/special_dir.hpp>
using pulp::platform::SpecialDir;

auto logs   = pulp::platform::special_dir(SpecialDir::logs) / "MyApp";
auto assets = pulp::platform::special_dir(SpecialDir::bundle_dir);   // resource root of this plugin or app
```

Kinds: `home`, `documents`, `desktop`, `music`, `app_data` (per user),
`app_data_shared` (all users), `config` (per-user settings), `temp`, `cache`,
`logs`, `current_executable`, `bundle_dir`, `user_presets`. The header lists the
exact path each platform returns.

- An empty path means the location is unavailable. Fail closed instead of falling
  back to a shared folder such as `/tmp`.
- Nothing is created or checked for existence.
- `bundle_dir` is the resource root of the binary that contains the Pulp code, so
  a plugin gets its own bundle, not its host's. `current_executable` is the host.
- Android resolves under the directory given to the existing `set_app_data_dir`
  hook. WebAssembly resolves inside the in-memory file system.
- `set_special_dir_override` redirects a kind for tests or for mobile hosts that
  learn their sandbox late.
- `special_dir_for(os, kind, inputs)` computes any platform's layout from an
  explicit environment, which is how the layouts are tested on every host.

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
