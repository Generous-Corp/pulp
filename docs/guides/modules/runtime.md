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
meters.publish({0.8f, 0.7f, 0.5f, 0.4f});

// UI thread (reader):
if (meters.has_new()) {
    auto data = meters.read();
    update_meter_display(data);
}
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
midi_queue.push(event);

// Consumer (UI thread):
MidiEvent ev;
while (midi_queue.pop(ev)) {
    handle_event(ev);
}
```

Use SpscQueue for ordered event streams where every event matters (MIDI, parameter automation points, UI commands).

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

PULP_LOG_INFO("Plugin loaded: {}", name);
PULP_LOG_WARN("Buffer underrun at sample {}", position);
PULP_LOG_ERROR("Failed to open device: {}", device_name);
```

Logging is safe to call from any thread. Output goes to stderr.

## Assertions

```cpp
#include <pulp/runtime/assert.hpp>

PULP_ASSERT(buffer.num_channels() > 0);
PULP_ASSERT_MSG(sample_rate > 0, "Sample rate must be positive");
```

Assertions are active in debug builds and removed in release builds.
