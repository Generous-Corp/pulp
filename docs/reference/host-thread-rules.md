# Host Thread Rules

`pulp::host` divides its API surface across three threads. Calling into
the wrong one either deadlocks, races, or — worst case — dereferences
freed memory. This reference exists to pin the contract down.

## The three threads

| Role           | What it does                                       | Runs on |
|----------------|----------------------------------------------------|---------|
| **UI thread** | Builds the graph, loads plugins, edits connections, drives automation | Your app's main/event thread |
| **Audio thread** | `SignalGraph::process()`, plugin `process()` callbacks | Driver-owned (CoreAudio, WASAPI, …) |
| **Worker thread** | `PluginSlot::load()` I/O, network, preset scans, deferred UI commits | Whatever pool the caller provides |

## `SignalGraph`

The graph uses an immutable-snapshot pattern. `process()` reads only
from a `CompiledGraph` published via `std::atomic` shared_ptr swap.
Mutators on the UI thread invalidate the snapshot, forcing silence
until `prepare()` is called to republish.

### UI-thread-only APIs (mutators)

All of the below modify `nodes_` or `connections_` and invalidate the
live snapshot. **Never call them from the audio thread.**

- `add_input_node` / `add_output_node` / `add_plugin_node` / `add_gain_node` / `add_midi_input_node` / `add_midi_output_node`
- `remove_node`
- `connect` / `connect_midi` / `connect_feedback` / `disconnect`
- `clear`
- `prepare` / `release`

### Audio-thread-safe APIs (read-only or snapshot-mutating)

- `process` — atomic-loads the snapshot once at entry; reads exclusively
  from it. Safe to call concurrently with any UI-thread mutator: in the
  worst case, the block after a mutation is silence until `prepare()`
  is re-called.
- `inject_midi` / `extract_midi` — mutate snapshot-owned MIDI scratch
  buffers; safe for the audio thread to call *before* the block
  starts (`inject_midi`) and *after* it ends (`extract_midi`). In
  typical flow, the UI thread injects MIDI and reads extracted events.

### UI-thread read-only accessors

These inspect `nodes_` / `connections_` directly. They must not be
called from the audio thread:

- `node` / `nodes` / `connections`
- `processing_order`
- `would_create_cycle`

### Either-thread (lock-free accessors against the snapshot)

- `latency_samples()` — returns a `std::atomic<int64_t>` load
- `node_latency_samples(id)` — snapshot-backed
- `node_gain(id)` — snapshot-backed

### Parameter control

- `set_node_parameter` / `get_node_parameter` forward to
  `PluginSlot::set_parameter` / `get_parameter`. These operate on the
  `nodes_` vector directly (via `node()`), so they are **UI-thread-only**.
  Per-block automation goes through `ParameterEventQueue` (Phase 0C)
  not through these.

## `PluginSlot`

Every loader must uphold the same contract:

- `prepare` / `release` — UI thread. Not real-time-safe; may allocate.
- `process` — audio thread. Real-time-safe. Receives per-block MIDI
  buffers and a `ParameterEventQueue`. Must not allocate, lock, or
  block.
- `set_parameter` / `get_parameter` — UI thread. May cause allocations,
  locks, or plugin-side dispatch.
- `save_state` / `restore_state` — UI thread. Slow; suitable for
  preset load/save.
- `has_editor` / `create_editor_view` / `destroy_editor_view` — UI
  thread. Editor views live in the host's native UI framework.
- `latency_samples` / `tail_samples` — may be called from either
  thread; loaders must make them lock-free or cache the value.
- `is_bypassed` / `set_bypass` — lock-free atomic flag on the host
  side; plugin-native bypass is loader-specific but typically cached.

## Parameter value domain

`PluginSlot::set_parameter(id, value)` and `get_parameter(id)` operate
in the **plain** (not normalized) parameter domain as advertised by
`HostParamInfo::min_value` / `max_value` / `default_value`. Loaders
that natively use normalized values (VST3) convert internally. Don't
normalize host-side.

Consumers must honor `HostParamInfo::flags`:

- `automatable = false` — `connect_automation` refuses the edge; `set_parameter` is still legal from the UI thread.
- `read_only = true` — plugin reports but doesn't accept writes; both `connect_automation` and `set_parameter` refuse.
- `stepped = true` — rounding is the caller's responsibility before writing.
- `is_bypass = true` — prefer the host's `set_bypass` instead.

## Snapshot publish protocol

1. UI thread calls a mutator (e.g. `connect(a, 0, b, 0)`).
2. Mutator appends to `connections_`, then `atomic_store(&live_, nullptr)`.
3. Audio thread's next `process()` call sees `live_ == nullptr` and
   writes silence to the output buffer, returning immediately.
4. UI thread calls `prepare(sample_rate, max_block_size)`. `prepare()`:
   - calls each plugin's `prepare(sample_rate, max_block_size)`
   - `compile_()` builds a fresh `CompiledGraph` from `nodes_` + `connections_`
   - atomic-stores the new snapshot
5. Audio thread's next `process()` call sees the new snapshot and
   resumes producing audio.

**Key invariant:** the snapshot owns everything it reads. Plugin
instances live behind `std::shared_ptr<PluginSlot>` held by both the
snapshot and `GraphNode`; if the UI thread removes a node while the
audio thread is mid-block, the plugin stays alive until the audio
thread drops its reference to the old snapshot. No dangling reads.

## When to `release()` vs `invalidate`

- `release()` — full teardown. Stops each plugin via
  `PluginSlot::release()`, nulls the snapshot. Pair with a subsequent
  `prepare()` or destroy the graph.
- Internal `invalidate_live_()` (triggered by mutators) — just nulls
  the snapshot. Doesn't touch plugin state.

If you want to change topology without an audio dropout, you can't —
yet. The current semantic is "mutation = silence until re-prepare".
A future iteration may add atomic-topology-swap (build a new
CompiledGraph eagerly in the mutator and swap without going through
nullptr); for v1, the simple semantic is intentional.
