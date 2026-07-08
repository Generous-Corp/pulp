# SequencerStateChannel — audio-safe engine→view state

`SequencerStateChannel` is Pulp's transport for a plugin's structured
**non-parameter** state — the bulk, observable state a step sequencer,
mod-matrix, or pattern engine holds *outside* its automatable parameter set
(e.g. a 12-lane × 32-step × 32-pattern grid, ~12k step records). Parameters keep
flowing through `StateStore` and the host; this channel carries everything else,
lock-free and allocation-free on the audio thread.

Header: `<pulp/state/sequencer_state_channel.hpp>` (channel + types) and
`<pulp/state/step_edit_reducer.hpp>` (the reference apply/drain logic). The UI
consumer is `StepGridView` (`<pulp/view/step_grid_view.hpp>`).

> **When NOT to use this.** If your per-parameter values should be host-automated
> or shown in the host's parameter list, they belong in `StateStore`, not here.
> If your control paints procedurally and turns raw pointer events into arbitrary
> multi-value writes (a free-form XY field, a "grow" slider), that is a
> **gesture canvas** — `DesignFrameView::Kind::custom` — not a step grid.
> `SequencerStateChannel` is for *griddable, cell-shaped* bulk state.

## The model

Four lock-free primitives with one ownership split:

| Direction | Carries | Primitive |
|-----------|---------|-----------|
| UI → engine | ordered edit commands | `SpscQueue<Command>` |
| engine → UI | ordered *applied* echoes | `SpscQueue<AppliedEdit>` |
| engine → UI | bulk / versioned snapshot | `TripleBuffer<Snapshot>` |
| engine → UI | high-rate playhead | `SeqLock<Playhead>` |

The **engine owns** the authoritative state. The **UI never mutates shared
state**: it submits commands and replays the engine's *applied echoes* onto its
own render copy, so both sides converge without either diffing the whole grid.
Each echo names the exact `DirtyRegion` it touched, so the UI invalidates only
what changed. A full `Snapshot` is published only for initial load, preset / bulk
replacement, and overflow recovery — never per cell edit.

Because `TripleBuffer` is latest-value and coalesces writes, **a `Snapshot` is
never an incremental change descriptor**: two edits published back-to-back
collapse to one snapshot and the first cell's dirty region is lost. Incremental
"what changed" flows only on the ordered echo stream; the snapshot is a coherent
resync point.

## Neutral producer contract (Q3/Q5)

The authoritative writer is **any single thread**. It need **not** be a
`pulp::format::Processor`, and neither `sequencer_state_channel.hpp` nor
`step_edit_reducer.hpp` includes anything from `<pulp/format/...>` or the
`StateStore` — only runtime primitives. A framework-neutral engine can own the
channel and drive it directly, exposing neutral types upward while the Pulp view
observes. This is the recommended pattern when the producer lives outside Pulp
(vs. `StateTree::clone_synced()`, which is the answer for *parameter/StateTree-
shaped* observable state but is coupled to `StateTree` semantics — for a neutral
producer, the binder owns a copy and reconciles).

A complete non-`Processor` producer:

```cpp
#include <pulp/state/sequencer_state_channel.hpp>
#include <pulp/state/step_edit_reducer.hpp>

struct MyEngine {                        // no Pulp base class, no <pulp/format/...>
    pulp::state::SequencerStateChannel channel;
    pulp::state::Snapshot snapshot{};
    pulp::state::Epoch epoch = 0;
    pulp::state::EngineSequence seq = 0;

    void on_audio_block() {
        // Apply queued UI edits + echo them; the reducer owns overflow recovery.
        pulp::state::drain_and_apply(channel, snapshot, epoch, seq);
        // ... your sequencing / DSP ...
    }
};
```

The UI binds a `StepGridView` to `engine.channel` and calls `pump()` each frame
(the view's FrameClock probe does this automatically once hosted).

## Parametric dimensions (SequencerConfig)

Dimensions are **compile-time**, via `SequencerConfig`, deliberately: the
`Snapshot` is a nested `std::array` and therefore trivially copyable, which is
what lets it publish through `TripleBuffer`/`SpscQueue` with no lock and no
allocation on the audio thread. Runtime dimensions would heap-allocate the
snapshot and forfeit that.

```cpp
template <std::size_t Lanes, std::size_t Steps, std::size_t Patterns,
          class Cell = StepCell, class Playhead = PlayheadState,
          std::size_t CommandDepth = 1024, std::size_t AppliedDepth = 1024>
struct SequencerConfig;
```

- **Dims are maximum capacity**, not "always fully populated." Active extent is
  explicit in the snapshot: `PatternState::length` (active steps),
  `Snapshot::active_lane_count`, `Snapshot::active_pattern_count`. Unused lanes /
  patterns are simply empty (zero cost — the arrays are fixed regardless).
- **`Cell`** defaults to `StepCell`; supply your own (trivially copyable,
  copy-assignable, default-constructible) to carry richer per-step data.
- **`Playhead`** defaults to the single-cursor `PlayheadState`; a polymetric
  engine can substitute a per-lane playhead. A custom playhead must expose
  `active_pattern`, `active_step`, and `playing` for `StepGridView` to project it.

Name the transport types via the channel's member typedefs
(`MyChannel::Snapshot`, `::Command`, `::AppliedEdit`, …). The reference
12×32×32 `StepCell` shape has namespace-scope convenience aliases
(`SequencerStateChannel`, `Snapshot`, …).

```cpp
using MyConfig  = pulp::state::SequencerConfig<8, 16, 4>;   // 8 lanes, 16 steps, 4 patterns
using MyChannel = pulp::state::SequencerStateChannelT<MyConfig>;
```

`StepGridView` generalizes the same way:
`StepGridViewT<MyConfig, MyCellPolicy>`. `CellPolicy` maps the cell type both to
a `CellVisual` (paint) **and** to `make_set_cell(old, enabled)` (the edit a click
applies) — a custom cell needs its own policy or it is a compile error, never a
silent blank/uneditable grid.

## Reducer + overflow recovery

`step_edit_reducer.hpp` owns the audio-side edit logic so a producer never
hand-rolls the protocol:

- `apply_step_edit<Config>(snapshot, command, engine_seq)` — the reference
  reducer (for `StepCell`). Mutates the snapshot for one command and returns the
  exact `AppliedEdit` echo the UI replays. A custom cell type supplies its own
  reducer with the same signature.
- `drain_and_apply<Config[, Reducer]>(channel, snapshot, epoch, engine_seq)` —
  drains the command FIFO, applies each command, publishes each echo, and **owns
  the overflow recovery**: if the applied FIFO is full, it republishes the
  authoritative snapshot and raises the resync bar (snapshot **before** the
  resync mark).

## Normative protocol rules (freeze contract)

These are part of the frozen contract — build against them:

1. **Exactly one UI-side consumer per channel.** The applied-echo FIFO is
   destructively consumed; two views on one channel corrupt each other. For a
   multi-view UI, put one owner (a render model) between channel and views.
   Debug builds assert on a second binding (the channel's consumer-claim token).
2. **Publish the snapshot before raising the resync bar.** Reversing the order
   makes the view resync repeatedly until the matching-epoch snapshot lands
   (self-healing but glitchy). `drain_and_apply` enforces the correct order.
3. **`engine_sequence` is strictly monotonic; the UI filters stale echoes by
   `<=`.** Sequence gaps are legal and undetected — loss detection is the
   producer's `try_push`/`try_submit` return value, nothing else. Never ignore a
   `false` from `audio_try_publish_applied` (doing so silently desyncs the UI
   forever) — use `drain_and_apply`, which makes that unrepresentable.
4. **`ui_read_latest_snapshot()` returns a reference valid only until the next
   read** on that channel (TripleBuffer front-slot stability). Copy out anything
   you retain across reads.
5. **Layout is source/API-compatible, not byte-persistent.** The `static_assert`s
   on the transport types flag accidental layout drift; they are **not** a promise
   that a `Snapshot` may be `memcpy`'d to disk or IPC. Persist via your own codec.
   `Snapshot::schema_version` is engine-defined and channel-opaque.

## Related

- [base-vs-modulated](base-vs-modulated.md) — the parameter modulation model (a
  different subsystem; `StateStore`, not this channel).
- `StateTree::clone_synced()` — observable change channel for parameter/StateTree
  state (coupled to `StateTree`; contrast the neutral-producer note above).
- `DesignFrameView::Kind::custom` — the gesture-canvas surface for free-form
  procedural controls.
