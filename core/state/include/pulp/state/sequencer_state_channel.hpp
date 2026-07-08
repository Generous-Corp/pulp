#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <type_traits>

#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/triple_buffer.hpp>

/// SequencerStateChannel is the audio-safe transport for a plugin's structured
/// NON-parameter state: the bulk, observable state a step sequencer /
/// mod-matrix / pattern engine holds outside its automatable parameter set
/// (e.g. a 12-lane x 32-step x 32-pattern grid, ~12k step records). Parameters
/// keep flowing through StateStore / the host; this channel carries the rest.
///
/// It is a THIN, typed bundle of the three lock-free primitives, with the
/// ownership split that keeps a large grid cheap to edit and cheap to render:
///
///   - UI -> audio: ordered edit commands            SpscQueue<Command>
///   - audio -> UI: ordered *applied* echoes         SpscQueue<AppliedEdit>
///   - audio -> UI: bulk/versioned state             TripleBuffer<Snapshot>
///   - audio -> UI: high-rate playhead               SeqLock<Playhead>
///
/// The authoritative writer (the "engine") OWNS the state; the UI never mutates
/// shared state — it submits commands and replays the engine's *applied* echoes
/// onto its own render copy, so both sides converge without either diffing the
/// whole grid every frame. Each applied echo names the exact DirtyRegion it
/// touched, so the UI invalidates only what changed. A full Snapshot is only
/// published for initial load, preset / bulk replacement, and overflow recovery
/// — never per cell edit (it is a large copy).
///
/// NEUTRAL PRODUCER CONTRACT: the authoritative writer is any single thread. It
/// need NOT be a pulp::format::Processor and this header includes nothing from
/// <pulp/format/...> or <pulp/state/store...> — only runtime primitives. A
/// non-Pulp engine can own the channel and drive it directly (see
/// step_edit_reducer.hpp for the reference apply/drain logic and the docs page
/// docs/reference/sequencer-state-channel.md for the full contract).
///
/// Because TripleBuffer is latest-value and coalesces intermediate writes, a
/// Snapshot must NEVER be used as an incremental change descriptor: two cell
/// edits published back-to-back would collapse to one snapshot and lose the
/// first cell's dirty region. Incremental "what changed" flows only on the
/// ordered AppliedEdit echo stream; the Snapshot is a coherent resync point.
///
/// This channel is pure transport: it invents no epochs or sequence numbers.
/// The engine controls Snapshot::epoch and *_sequence values; the channel moves
/// the bytes and exposes the resync-epoch signal + FIFO telemetry.
///
/// DIMENSIONS ARE COMPILE-TIME (SequencerConfig), by design: the Snapshot is a
/// nested std::array and therefore trivially copyable, which is the property
/// that lets TripleBuffer/SpscQueue publish it lock-free and allocation-free on
/// the audio thread. Config dims are MAXIMUM CAPACITY; active extent is explicit
/// in the Snapshot (PatternState::length, active_lane_count, active_pattern_count).
namespace pulp::state {

// ── Shared scalar types (dimension-independent) ──────────────────────────────
using Epoch = std::uint64_t;
using EngineSequence = std::uint64_t;
using ClientSequence = std::uint64_t;
using EditTransactionId = std::uint32_t;

/// One step in one lane. A trivially-copyable POD so patterns memcpy cleanly.
/// `flags` bit 0 is "enabled"; higher bits are reserved for tie/accent/etc.
/// This is the reference cell; a Config may substitute any trivially-copyable,
/// copy-assignable, default-constructible cell type.
struct StepCell {
    std::uint8_t flags = 0;
    std::uint8_t velocity = 100;      // 0..127
    std::uint8_t probability = 127;   // 0..127
    std::int8_t pitch_offset = 0;     // semitones
    std::uint16_t gate_ticks = 24;    // gate length in ticks
    std::uint8_t ratchet = 1;         // sub-hits per step
    std::uint8_t reserved = 0;

    static constexpr std::uint8_t kEnabledBit = 0x01;
    constexpr bool enabled() const { return (flags & kEnabledBit) != 0; }
};

/// Volatile, high-rate transport/playhead state. Trivially copyable for SeqLock.
/// This is the reference single-cursor playhead; a Config may substitute a
/// richer (e.g. per-lane) trivially-copyable playhead type.
struct PlayheadState {
    std::uint64_t sample_time = 0;
    double ppq_position = 0.0;
    std::uint32_t block_index = 0;
    std::uint8_t active_pattern = 0;
    std::uint8_t active_step = 0;
    std::uint8_t playing = 0;          // bool as byte for trivial-copy stability
    std::uint8_t reserved = 0;
};
static_assert(std::is_trivially_copyable_v<PlayheadState>,
              "PlayheadState must be trivially copyable for SeqLock");

// ── SequencerConfig — the compile-time dimension/type bundle ─────────────────
///
/// Lanes/Steps/Patterns are MAXIMUM CAPACITY (active extent lives in the
/// Snapshot). Cell/Playhead default to the reference types. CommandDepth/
/// AppliedDepth size the SPSC FIFOs.
template <std::size_t Lanes, std::size_t Steps, std::size_t Patterns,
          class Cell = StepCell, class Playhead = PlayheadState,
          std::size_t CommandDepth = 1024, std::size_t AppliedDepth = 1024>
struct SequencerConfig {
    static constexpr std::size_t lanes = Lanes;
    static constexpr std::size_t steps = Steps;
    static constexpr std::size_t patterns = Patterns;
    static constexpr std::size_t command_depth = CommandDepth;
    static constexpr std::size_t applied_depth = AppliedDepth;
    using cell_type = Cell;
    using playhead_type = Playhead;

    static_assert(Lanes >= 1 && Steps >= 1 && Patterns >= 1,
                  "dimensions must be >= 1");
    static_assert(Lanes <= 255 && Steps <= 255 && Patterns <= 255,
                  "wire indices are uint8_t; dims are capped at 255");
    static_assert(CommandDepth > 0 && AppliedDepth > 0, "FIFO depths must be > 0");
    // NOTE: do NOT require trivially-default-constructible — the reference
    // StepCell has default member initializers, so it is trivially copyable but
    // not trivially default constructible. The operational contract is:
    // trivially copyable (memcpy/TripleBuffer/FIFO), copy-assignable
    // (TripleBuffer::write assigns), and cheaply default-constructible
    // (SpscQueue::try_pop default-constructs the element on the audio thread).
    static_assert(std::is_trivially_copyable_v<Cell> &&
                      std::is_copy_assignable_v<Cell> &&
                      std::is_default_constructible_v<Cell>,
                  "Cell must be trivially copyable, copy-assignable, and cheaply "
                  "default-constructible (its default ctor runs on audio-thread pop)");
    static_assert(std::is_trivially_copyable_v<Playhead> &&
                      std::is_copy_assignable_v<Playhead>,
                  "Playhead must be trivially copyable + copy-assignable (SeqLock)");
};

// ── Reference dimensions (sized to the reference sequencer shape) ────────────
inline constexpr std::uint8_t kPatternCount = 32;
inline constexpr std::uint8_t kLaneCount = 12;
inline constexpr std::uint8_t kStepCount = 32;

// FIFO depths. Commands and echoes are small and drained every UI/audio tick;
// these are generous relative to a realistic edit burst. Overflow is handled
// (command submit fails; echo overflow forces a snapshot resync), never UB.
inline constexpr std::size_t kCommandQueueCapacity = 1024;
inline constexpr std::size_t kAppliedQueueCapacity = 1024;

// ── Dirty-region descriptor (UI invalidation granularity) ────────────────────
enum class DirtyKind : std::uint8_t {
    None,
    FullSnapshot,
    Pattern,
    Lane,
    StepRange,
    Cell,
};

struct DirtyRegion {
    DirtyKind kind = DirtyKind::None;
    std::uint8_t pattern = 0;
    std::uint8_t lane = 0;
    std::uint8_t first_step = 0;
    std::uint8_t step_count = 0;
};

// ── UI -> audio: edit commands ───────────────────────────────────────────────
enum class StepEditKind : std::uint8_t {
    SetCell,
    Clear,
    RandomizeLane,
    SetPatternLength,
    SwitchPattern,
};

enum class ClearScope : std::uint8_t { Cell, Lane, Pattern, All };

/// Host-undo gesture bracketing hint. The channel does not own an undo log; an
/// undo manager above the channel groups edits by (transaction_id, phase).
enum class GesturePhase : std::uint8_t { None, Begin, Update, End, Cancel };

/// SetCell carries a full cell by value, so it is templated on the Config's
/// cell type. The other edits carry only indices and are dimension-independent.
template <class Config>
struct SetCellEditT {
    std::uint8_t pattern = 0;
    std::uint8_t lane = 0;
    std::uint8_t step = 0;
    typename Config::cell_type cell{};
};
struct ClearEdit {
    ClearScope scope = ClearScope::Cell;
    std::uint8_t pattern = 0;
    std::uint8_t lane = 0;
    std::uint8_t step = 0;
};
struct RandomizeLaneEdit {
    std::uint8_t pattern = 0;
    std::uint8_t lane = 0;
    std::uint32_t seed = 0;
    std::uint8_t density = 64;         // 0..127 probability of a hit per step
    std::uint8_t min_velocity = 64;
    std::uint8_t max_velocity = 127;
};
struct SetPatternLengthEdit {
    std::uint8_t pattern = 0;
    std::uint8_t length = 0;           // active step count; 0 = full (Config::steps)
};
struct SwitchPatternEdit {
    std::uint8_t pattern = 0;
};

template <class Config>
struct StepEditCommandT {
    ClientSequence client_sequence = 0;
    EditTransactionId transaction_id = 0;
    GesturePhase gesture_phase = GesturePhase::None;
    StepEditKind kind = StepEditKind::SetCell;

    union Payload {
        SetCellEditT<Config> set_cell;
        ClearEdit clear;
        RandomizeLaneEdit randomize_lane;
        SetPatternLengthEdit set_pattern_length;
        SwitchPatternEdit switch_pattern;
        constexpr Payload() : set_cell{} {}
    } payload{};
};

// ── Authoritative state model ────────────────────────────────────────────────
template <class Config>
struct PatternStateT {
    std::uint8_t length = static_cast<std::uint8_t>(Config::steps);  // active steps (<= Steps)
    std::array<std::array<typename Config::cell_type, Config::steps>, Config::lanes> lanes{};
};

/// The full non-param state, published for bulk replacement / resync only.
/// Config dims are capacity; active_* fields carry the active extent.
template <class Config>
struct SnapshotT {
    std::uint32_t schema_version = 1;
    Epoch epoch = 0;                     // engine-controlled; monotonic per publish
    EngineSequence engine_sequence = 0;  // last applied edit folded into this snapshot
    std::uint8_t active_pattern = 0;
    std::uint8_t active_lane_count = static_cast<std::uint8_t>(Config::lanes);
    std::uint8_t active_pattern_count = static_cast<std::uint8_t>(Config::patterns);
    std::array<PatternStateT<Config>, Config::patterns> patterns{};
};

// ── audio -> UI: applied echoes ──────────────────────────────────────────────

/// The exact cells the engine wrote for a step-range edit. Randomize echoes its
/// resulting cells here rather than the seed, so the UI never re-runs (and never
/// drifts from) the engine's algorithm.
template <class Config>
struct StepRangeAppliedT {
    std::uint8_t pattern = 0;
    std::uint8_t lane = 0;
    std::uint8_t first_step = 0;
    std::uint8_t step_count = 0;
    std::array<typename Config::cell_type, Config::steps> cells{};
};

enum class AppliedEditKind : std::uint8_t {
    StepRangeChanged,
    PatternLengthChanged,
    ActivePatternChanged,
    CommandRejected,
};

template <class Config>
struct AppliedEditT {
    EngineSequence engine_sequence = 0;  // monotonic; UI filters stale echoes by this
    Epoch snapshot_epoch = 0;            // snapshot this echo is relative to
    ClientSequence client_sequence = 0;  // echoes the originating command's id
    EditTransactionId transaction_id = 0;
    AppliedEditKind kind = AppliedEditKind::StepRangeChanged;
    DirtyRegion dirty{};

    union Payload {
        StepRangeAppliedT<Config> step_range;
        SetPatternLengthEdit pattern_length;
        SwitchPatternEdit active_pattern;
        std::uint32_t reject_reason;
        constexpr Payload() : step_range{} {}
    } payload{};
};

// ── The channel ──────────────────────────────────────────────────────────────
///
/// Ownership: the engine (audio) thread is the single reader of the command
/// queue and the single writer of the applied queue / snapshot / playhead. The
/// UI/message thread is the single writer of the command queue and the single
/// reader of the applied queue / snapshot / playhead. Do not call the UI-side
/// methods from the audio thread or vice versa — each FIFO is strictly
/// single-reader/single-writer.
///
/// EXACTLY ONE UI-side consumer per channel: the applied-echo FIFO is
/// destructively consumed, so two views bound to one channel would each see a
/// random interleaving of echoes and corrupt each other. For a multi-view UI put
/// one owner (a render model) between the channel and the views. Debug builds
/// enforce this via a consumer-claim token (see debug_claim_ui_consumer).
template <class Config>
class SequencerStateChannelT {
public:
    using ConfigType = Config;
    using Snapshot = SnapshotT<Config>;
    using PatternState = PatternStateT<Config>;
    using Command = StepEditCommandT<Config>;
    using AppliedEdit = AppliedEditT<Config>;
    using StepRangeApplied = StepRangeAppliedT<Config>;
    using SetCellEdit = SetCellEditT<Config>;
    using Playhead = typename Config::playhead_type;

    SequencerStateChannelT() = default;

    // ── UI / message thread ──────────────────────────────────────────────────

    /// Submit an edit command. Lock-free, allocation-free. Returns false if the
    /// command FIFO is full (the UI should coalesce / retry, never block).
    bool ui_try_submit(const Command& command) noexcept {
        return commands_.try_push(command);
    }

    /// Pop the next applied echo to replay onto the UI render copy, or nullopt.
    /// Lock-free, allocation-free.
    std::optional<AppliedEdit> ui_try_pop_applied() noexcept {
        return applied_.try_pop();
    }

    /// Coherent read of the latest playhead. Lock-free, allocation-free.
    Playhead ui_read_playhead() const noexcept { return playhead_.read(); }

    /// The latest full snapshot. Lock-free but O(snapshot size); call only when
    /// resyncing (see ui_resync_required_epoch), not every frame.
    ///
    /// LIFETIME: the reference is valid only until the next ui_read_latest_snapshot
    /// call on this channel (TripleBuffer front-slot stability). Copy out anything
    /// you need to retain across reads.
    const Snapshot& ui_read_latest_snapshot() noexcept { return snapshot_.read(); }

    /// The snapshot epoch the UI must reach before trusting incremental echoes
    /// again. When this exceeds the UI's local snapshot epoch (or the applied
    /// FIFO overflow count moved), the UI must resync from ui_read_latest_snapshot
    /// and drop echoes with engine_sequence <= the snapshot's engine_sequence.
    Epoch ui_resync_required_epoch() const noexcept {
        return resync_epoch_.load(std::memory_order_acquire);
    }

    runtime::SpscQueueTelemetry ui_applied_telemetry() const noexcept {
        return applied_.telemetry();
    }
    runtime::SpscQueueTelemetry ui_command_telemetry() const noexcept {
        return commands_.telemetry();
    }

    /// Debug-only single-UI-consumer guard. The UI side claims the channel on
    /// bind and releases on unbind; a second concurrent claim trips an assert in
    /// the caller. No-op (always succeeds) in release builds.
    bool debug_claim_ui_consumer() noexcept {
#ifndef NDEBUG
        bool expected = false;
        return ui_consumer_claimed_.compare_exchange_strong(expected, true,
                                                            std::memory_order_acq_rel);
#else
        return true;
#endif
    }
    void debug_release_ui_consumer() noexcept {
#ifndef NDEBUG
        ui_consumer_claimed_.store(false, std::memory_order_release);
#endif
    }

    // ── Engine (audio) thread ─────────────────────────────────────────────────

    /// Pop the next pending edit command to apply. Lock-free, allocation-free.
    std::optional<Command> audio_try_pop_command() noexcept {
        return commands_.try_pop();
    }

    /// Publish an applied echo. Lock-free, allocation-free. Returns false if the
    /// applied FIFO is full — the engine must then publish a fresh Snapshot and
    /// call audio_mark_resync_required(snapshot.epoch) so the UI recovers instead
    /// of missing the lost echoes. (step_edit_reducer.hpp's drain_and_apply owns
    /// this recovery so producers never hand-roll it wrong.)
    bool audio_try_publish_applied(const AppliedEdit& edit) noexcept {
        return applied_.try_push(edit);
    }

    /// Publish the full authoritative snapshot (initial load / preset / bulk /
    /// resync). Lock-free but O(snapshot size) — never call per cell edit.
    void audio_publish_snapshot(const Snapshot& snapshot) noexcept {
        snapshot_.write(snapshot);
    }

    /// Publish the latest playhead. Lock-free, allocation-free; call per block.
    void audio_publish_playhead(const Playhead& playhead) noexcept {
        playhead_.write(playhead);
    }

    /// Signal that incremental echoes were lost and the UI must resync to at
    /// least @p epoch. Monotonic: a later resync request never lowers the bar.
    /// Publish the snapshot BEFORE calling this (reversing the order live-locks
    /// the UI into repeated resyncs until the matching-epoch snapshot lands).
    void audio_mark_resync_required(Epoch epoch) noexcept {
        Epoch prev = resync_epoch_.load(std::memory_order_relaxed);
        while (epoch > prev &&
               !resync_epoch_.compare_exchange_weak(prev, epoch,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed)) {
            // prev reloaded by compare_exchange_weak; retry until we win or
            // another (larger) value already satisfies the bar.
        }
    }

private:
    runtime::SpscQueue<Command, Config::command_depth> commands_{};
    runtime::SpscQueue<AppliedEdit, Config::applied_depth> applied_{};
    runtime::TripleBuffer<Snapshot> snapshot_{};
    runtime::SeqLock<Playhead> playhead_{};
    std::atomic<Epoch> resync_epoch_{0};
#ifndef NDEBUG
    std::atomic<bool> ui_consumer_claimed_{false};
#endif
};

// ── Reference config + aliases ───────────────────────────────────────────────
///
/// The canonical way to name the transport types is via the channel's member
/// typedefs (e.g. MyChannel::Snapshot). These namespace-scope aliases are a
/// convenience for the reference 12x32x32 StepCell shape used by the built-in
/// StepGridView / StepSequencer example.
using ReferenceSequencerConfig =
    SequencerConfig<kLaneCount, kStepCount, kPatternCount, StepCell>;

using SequencerStateChannel = SequencerStateChannelT<ReferenceSequencerConfig>;
using Snapshot = SnapshotT<ReferenceSequencerConfig>;
using PatternState = PatternStateT<ReferenceSequencerConfig>;
using StepEditCommand = StepEditCommandT<ReferenceSequencerConfig>;
using AppliedEdit = AppliedEditT<ReferenceSequencerConfig>;
using StepRangeApplied = StepRangeAppliedT<ReferenceSequencerConfig>;
using SetCellEdit = SetCellEditT<ReferenceSequencerConfig>;

// ── Frozen layout tripwires (source/API compat only — NOT byte persistence) ──
// These flag accidental layout drift of the reference transport types. They are
// NOT a promise that a Snapshot may be memcpy'd to disk/IPC; persist via a codec.
static_assert(std::is_trivially_copyable_v<StepCell>);
static_assert(std::is_trivially_copyable_v<DirtyRegion>);
static_assert(std::is_trivially_copyable_v<StepEditCommand> &&
                  std::is_copy_assignable_v<StepEditCommand> &&
                  std::is_default_constructible_v<StepEditCommand>,
              "Command must be trivially copyable + copy-assignable + default-constructible "
              "for the SPSC FIFO");
static_assert(std::is_trivially_copyable_v<AppliedEdit> &&
                  std::is_copy_assignable_v<AppliedEdit> &&
                  std::is_default_constructible_v<AppliedEdit>,
              "AppliedEdit must be trivially copyable + copy-assignable + default-constructible "
              "for the SPSC FIFO");
static_assert(std::is_trivially_copyable_v<Snapshot> &&
                  std::is_copy_assignable_v<Snapshot> &&
                  std::is_default_constructible_v<Snapshot>,
              "Snapshot must be trivially copyable + copy-assignable for TripleBuffer");

} // namespace pulp::state
