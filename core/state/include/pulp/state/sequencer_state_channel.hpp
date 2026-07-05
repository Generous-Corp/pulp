#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <type_traits>

#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/triple_buffer.hpp>

/// EXPERIMENTAL — API not yet frozen.
///
/// SequencerStateChannel is the audio-safe transport for a plugin's structured
/// NON-parameter state: the kind of bulk, observable state a step sequencer /
/// mod-matrix / pattern engine holds outside its automatable parameter set
/// (e.g. a 12-lane x 32-step x 32-pattern grid, ~12k step records). Parameters
/// keep flowing through StateStore / the host; this channel carries the rest.
///
/// It is deliberately a THIN, typed bundle of the three lock-free primitives,
/// with the ownership split that keeps a 12k-cell grid cheap to edit and cheap
/// to render:
///
///   - UI -> audio: ordered edit commands            SpscQueue<StepEditCommand>
///   - audio -> UI: ordered *applied* echoes         SpscQueue<AppliedEdit>
///   - audio -> UI: bulk/versioned state             TripleBuffer<Snapshot>
///   - audio -> UI: high-rate playhead               SeqLock<PlayheadState>
///
/// The audio thread OWNS the authoritative state. The UI never mutates shared
/// state: it submits commands and replays the audio thread's *applied* echoes
/// onto its own render copy, so both sides converge without either diffing the
/// whole grid every frame. Each applied echo names the exact DirtyRegion it
/// touched, so the UI invalidates only what changed. A full Snapshot is only
/// published for initial load, preset / bulk replacement, and overflow recovery
/// — never per cell edit (it is a large copy).
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
namespace pulp::state {

// ── Fixed dimensions (sized to the reference sequencer shape) ────────────────
inline constexpr std::uint8_t kPatternCount = 32;
inline constexpr std::uint8_t kLaneCount = 12;
inline constexpr std::uint8_t kStepCount = 32;

// FIFO depths. Commands and echoes are small and drained every UI/audio tick;
// these are generous relative to a realistic edit burst. Overflow is handled
// (command submit fails; echo overflow forces a snapshot resync), never UB.
inline constexpr std::size_t kCommandQueueCapacity = 1024;
inline constexpr std::size_t kAppliedQueueCapacity = 1024;

using Epoch = std::uint64_t;
using EngineSequence = std::uint64_t;
using ClientSequence = std::uint64_t;
using EditTransactionId = std::uint32_t;

// ── Authoritative state model ────────────────────────────────────────────────

/// One step in one lane. A trivially-copyable POD so patterns memcpy cleanly.
/// `flags` bit 0 is "enabled"; higher bits are reserved for tie/accent/etc.
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

struct PatternState {
    std::uint8_t length = kStepCount;  // active step count (<= kStepCount)
    std::array<std::array<StepCell, kStepCount>, kLaneCount> lanes{};
};

/// The full non-param state, published for bulk replacement / resync only.
struct Snapshot {
    std::uint32_t schema_version = 1;
    Epoch epoch = 0;                   // engine-controlled; monotonic per publish
    EngineSequence engine_sequence = 0;  // last applied edit folded into this snapshot
    std::uint8_t active_pattern = 0;
    std::array<PatternState, kPatternCount> patterns{};
};

/// Volatile, high-rate transport/playhead state. Trivially copyable for SeqLock.
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

struct SetCellEdit {
    std::uint8_t pattern = 0;
    std::uint8_t lane = 0;
    std::uint8_t step = 0;
    StepCell cell{};
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
    std::uint8_t length = kStepCount;
};
struct SwitchPatternEdit {
    std::uint8_t pattern = 0;
};

struct StepEditCommand {
    ClientSequence client_sequence = 0;
    EditTransactionId transaction_id = 0;
    GesturePhase gesture_phase = GesturePhase::None;
    StepEditKind kind = StepEditKind::SetCell;

    union Payload {
        SetCellEdit set_cell;
        ClearEdit clear;
        RandomizeLaneEdit randomize_lane;
        SetPatternLengthEdit set_pattern_length;
        SwitchPatternEdit switch_pattern;
        constexpr Payload() : set_cell{} {}
    } payload{};
};
static_assert(std::is_trivially_copyable_v<StepEditCommand>,
              "StepEditCommand must be trivially copyable for the SPSC FIFO");

// ── audio -> UI: applied echoes ──────────────────────────────────────────────

/// The exact cells the audio thread wrote for a step-range edit. Randomize
/// echoes its resulting cells here rather than the seed, so the UI never re-runs
/// (and never drifts from) the audio thread's algorithm.
struct StepRangeApplied {
    std::uint8_t pattern = 0;
    std::uint8_t lane = 0;
    std::uint8_t first_step = 0;
    std::uint8_t step_count = 0;
    std::array<StepCell, kStepCount> cells{};
};

enum class AppliedEditKind : std::uint8_t {
    StepRangeChanged,
    PatternLengthChanged,
    ActivePatternChanged,
    CommandRejected,
};

struct AppliedEdit {
    EngineSequence engine_sequence = 0;  // monotonic; UI filters stale echoes by this
    Epoch snapshot_epoch = 0;            // snapshot this echo is relative to
    ClientSequence client_sequence = 0;  // echoes the originating command's id
    EditTransactionId transaction_id = 0;
    AppliedEditKind kind = AppliedEditKind::StepRangeChanged;
    DirtyRegion dirty{};

    union Payload {
        StepRangeApplied step_range;
        SetPatternLengthEdit pattern_length;
        SwitchPatternEdit active_pattern;
        std::uint32_t reject_reason;
        constexpr Payload() : step_range{} {}
    } payload{};
};
static_assert(std::is_trivially_copyable_v<AppliedEdit>,
              "AppliedEdit must be trivially copyable for the SPSC FIFO");

// ── The channel ──────────────────────────────────────────────────────────────
///
/// Ownership: the audio thread is the single reader of the command queue and the
/// single writer of the applied queue / snapshot / playhead. The UI/message
/// thread is the single writer of the command queue and the single reader of the
/// applied queue / snapshot / playhead. Do not call the UI-side methods from the
/// audio thread or vice versa — each FIFO is strictly single-reader/single-writer.
class SequencerStateChannel {
public:
    SequencerStateChannel() = default;

    // ── UI / message thread ──────────────────────────────────────────────────

    /// Submit an edit command. Lock-free, allocation-free. Returns false if the
    /// command FIFO is full (the UI should coalesce / retry, never block).
    bool ui_try_submit(const StepEditCommand& command) noexcept {
        return commands_.try_push(command);
    }

    /// Pop the next applied echo to replay onto the UI render copy, or nullopt.
    /// Lock-free, allocation-free.
    std::optional<AppliedEdit> ui_try_pop_applied() noexcept {
        return applied_.try_pop();
    }

    /// Coherent read of the latest playhead. Lock-free, allocation-free.
    PlayheadState ui_read_playhead() const noexcept { return playhead_.read(); }

    /// The latest full snapshot. Lock-free but O(snapshot size); call only when
    /// resyncing (see ui_resync_required_epoch), not every frame.
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

    // ── Audio thread ──────────────────────────────────────────────────────────

    /// Pop the next pending edit command to apply. Lock-free, allocation-free.
    std::optional<StepEditCommand> audio_try_pop_command() noexcept {
        return commands_.try_pop();
    }

    /// Publish an applied echo. Lock-free, allocation-free. Returns false if the
    /// applied FIFO is full — the engine must then publish a fresh Snapshot and
    /// call audio_mark_resync_required(snapshot.epoch) so the UI recovers instead
    /// of missing the lost echoes.
    bool audio_try_publish_applied(const AppliedEdit& edit) noexcept {
        return applied_.try_push(edit);
    }

    /// Publish the full authoritative snapshot (initial load / preset / bulk /
    /// resync). Lock-free but O(snapshot size) — never call per cell edit.
    void audio_publish_snapshot(const Snapshot& snapshot) noexcept {
        snapshot_.write(snapshot);
    }

    /// Publish the latest playhead. Lock-free, allocation-free; call per block.
    void audio_publish_playhead(const PlayheadState& playhead) noexcept {
        playhead_.write(playhead);
    }

    /// Signal that incremental echoes were lost and the UI must resync to at
    /// least @p epoch. Monotonic: a later resync request never lowers the bar.
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
    runtime::SpscQueue<StepEditCommand, kCommandQueueCapacity> commands_{};
    runtime::SpscQueue<AppliedEdit, kAppliedQueueCapacity> applied_{};
    runtime::TripleBuffer<Snapshot> snapshot_{};
    runtime::SeqLock<PlayheadState> playhead_{};
    std::atomic<Epoch> resync_epoch_{0};
};

} // namespace pulp::state
