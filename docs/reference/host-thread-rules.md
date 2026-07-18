# Host Thread Rules

`pulp::host` divides its API surface across three threads. Calling into
the wrong one either deadlocks, races, or — worst case — dereferences
freed memory. This reference exists to pin the contract down.

## The three threads

| Role           | What it does                                       | Runs on |
|----------------|----------------------------------------------------|---------|
| **UI thread** | Builds the graph, loads plugins, edits connections, drives automation | Your app's main/event thread |
| **Audio thread** | `SignalGraph::process()`, plugin `process()` callbacks | Driver-owned (CoreAudio, WASAPI, …) |
| **Worker thread** | `PluginSlot::load()` I/O, network, preset scans, deferred UI commits, single-producer `SignalGraph::pump_anticipation()` | Whatever pool the caller provides |

## `SignalGraph`

The graph uses an immutable-snapshot pattern. `process()` reads only
from a `CompiledGraph` published through `runtime::Slot`. Ordinary mutators on
the UI thread invalidate the snapshot, forcing silence until `prepare()` is
called to republish. A `begin_swap_edit()` / `prepare_swap()` transaction is the
gap-free exception: its supported mutators leave the old snapshot playing while
the control thread compiles and atomically publishes the candidate.

### UI-thread-only APIs (mutators)

All of the below modify `nodes_` or `connections_` and invalidate the
live snapshot. **Never call them from the audio thread.**

- `add_input_node` / `add_output_node` / `add_plugin_node` / `add_gain_node` / `add_midi_input_node` / `add_midi_output_node`
- `remove_node`
- `connect` / `connect_midi` / `connect_feedback` / `disconnect`
- `clear`
- `prepare` / `release`
- `begin_swap_edit` / `prepare_swap` / `abort_swap_edit`

### UI/control-thread live scalar updates

- `set_node_gain(id, linear_gain)` writes the UI-owned graph state and,
  when a prepared snapshot is live, stores the same value into the
  snapshot-owned `std::atomic<float>` used by `process()`. It is safe
  to call while the audio thread is processing, but it is still a
  control-thread API and must not race other graph/topology mutations.

### Audio-thread-safe APIs (read-only or snapshot-mutating)

- `process` — atomic-loads the snapshot once at entry; reads exclusively
  from it. Safe to call concurrently with any UI-thread mutator: in the
  worst case, the block after a mutation is silence until `prepare()`
  is re-called.
- `inject_midi` / `extract_midi` — publish/read MIDI through
  snapshot-owned lock-free mailboxes; safe to call concurrently with
  `process()`. Each `MidiInput` has exactly one injection writer: either a
  control-side producer, or the audio thread immediately before `process()`,
  never both concurrently. Injection is allocation-free after `prepare()`, is
  latest-wins when several blocks are published before processing, and is
  consumed exactly once. Publication sequence numbers never use zero (the
  unpublished sentinel), including after 64-bit wrap. A `false` overflow result
  still publishes the bounded prefix. Preserved `MidiInput` node IDs keep an
  unconsumed publication across an ingress-only gap-free `prepare_swap()`. MIDI
  extraction remains a
  control-side read. If either the current authoring topology or the live
  snapshot contains `MidiOutput`, the edit uses the eager-prepare fallback;
  that specific refusal keeps the old snapshot live so the control
  thread can drain pending egress before calling ordinary `prepare()`.
- `inject_parameter_events` — publishes one block of sample-offset parameter
  events through a snapshot-owned lock-free mailbox. It is safe to call
  concurrently with `process()` from one control-side writer. The latest
  publication replaces any older unconsumed publication and is consumed once
  by the next successful block. Injected events are appended after graph
  automation, so graph automation keeps queue-capacity priority and an
  injected event wins a stable-sort tie at the same sample offset. A `false`
  return means the node is unavailable or the source queue already overflowed;
  any retained source prefix is still published. If graph automation leaves
  insufficient destination capacity, the fixed queue drops the injected tail.

The TSan-oriented graph tests pin the combined contract: one thread may run
`SignalGraph::process()` while a control thread calls `set_node_gain()`,
`inject_midi()`, `extract_midi()`, and `inject_parameter_events()` against the
prepared snapshot. See
`pulp-test-host-signal-graph` filter
`"[host][graph][threading][race][tsan]"`.

### Worker-thread producer APIs

- `pump_anticipation(max_blocks)` — renders an eligible anticipative interior
  into the live snapshot's lane. It is guarded against concurrent or reentrant
  producer calls, but the host must still treat it as a single-producer API.
  Stop calling it and join the producer thread before any `prepare()` or graph
  mutation; the snapshot reader pin keeps the compiled graph alive, but it does
  not make the shared plugin instances safe to re-prepare while the producer is
  rendering them.

### UI-thread read-only accessors

These inspect `nodes_` / `connections_` directly. They must not be
called from the audio thread:

- `node` / `nodes` / `connections`
- `node_gain(id)`
- `processing_order`
- `would_create_cycle`

### Either-thread (lock-free accessors against the snapshot)

- `latency_samples()` — returns a `std::atomic<int64_t>` load
- `node_latency_samples(id)` — snapshot-backed

### Parameter control

- `set_node_parameter` / `get_node_parameter` forward to
  `PluginSlot::set_parameter` / `get_parameter`. These operate on the
  `nodes_` vector directly (via `node()`), so they are **UI-thread-only**.
  Per-block automation goes through `ParameterEventQueue`, either from graph
  automation connections or `inject_parameter_events()`, not through these.

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

## Timeline graph binding

`TimelineGraphPlaybackBinding::prepare()` is control-thread work. It preflights
the exact candidate routed plan, reconciles ItemId-keyed audio and MIDI nodes,
and prepares the graph. Stop `process()` before calling it or destroying the
binding; the referenced `SignalGraph` and `PlaybackProgramStore` must outlive
the binding.

`TimelineGraphPlaybackBinding::process()` is the only audio-thread entry point.
It pins one `PlaybackProgramBlock` for the whole callback, uses one exact
`TransportSnapshot` for every track (including parallel graph workers), renders
notes outside custom audio nodes, and injects those events into the track's
stable `MidiInput`. Program publication goes through `PlaybackProgramStore`;
never use `set_custom_node_state()` to update a timeline renderer program.

## Snapshot publish protocol

1. UI thread calls an ordinary mutator (e.g. `connect(a, 0, b, 0)`).
2. Mutator appends to `connections_`, then unpublishes the live slot.
3. Audio thread's next `process()` call sees no live snapshot and
   writes silence to the output buffer, returning immediately.
4. UI thread calls `prepare(sample_rate, max_block_size)`. `prepare()`:
   - calls each plugin's `prepare(sample_rate, max_block_size)`
   - `compile_()` builds a fresh `CompiledGraph` from `nodes_` + `connections_`
   - publishes the new snapshot through `runtime::Slot`
5. Audio thread's next `process()` call sees the new snapshot and
   resumes producing audio.

**Key invariant:** the snapshot owns everything it reads. Plugin
instances live behind `std::shared_ptr<PluginSlot>` held by both the
snapshot and `GraphNode`; if the UI thread removes a node while the
audio thread is mid-block, the plugin stays alive until the audio
thread drops its reference to the old snapshot. No dangling reads.

## When to `release()` vs `invalidate`

- `release()` — full teardown. Unpublishes the live snapshot first,
  waits for any in-flight snapshot reader to drop it, then stops each
  plugin via `PluginSlot::release()`. Pair with a subsequent `prepare()`
  or destroy the graph.
- Internal `invalidate_live_()` (triggered by mutators) — just nulls
  the snapshot. Doesn't touch plugin state.

For an eligible topology change without an audio dropout, call
`begin_swap_edit()`, make the supported edits on that same control thread, then
call `prepare_swap(sample_rate, max_block_size)`. The old snapshot keeps playing
through candidate compilation. `Swapped` means the candidate was published
gap-free; `NeedsEagerPrepare` means the transaction failed closed and the caller
must use ordinary `prepare()`. Eligibility requires unchanged node/plugin/custom
instance contracts, sample rate, block size, total latency, and feed-forward PDC
delay structure. Matching PDC history is shared by private connection identity;
feedback, MIDI output, smoothed sparse automation, anticipation,
latency-changing edits, and disconnect/reconnect of a delayed edge use the eager
path. Ingress-only MIDI edges are eligible because a stable `MidiInput` shares
its mailbox and consumed-sequence state across snapshots.
