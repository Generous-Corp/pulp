#pragma once

/// @file generated_event_source.hpp
/// Bounded handoff for event batches produced ahead of the audio playhead.
///
/// The producer may revise private staged batches until a quantized commit
/// freezes their span. Commit copies complete batches into a separate SPSC
/// ring, so the audio consumer never observes or races a revision. Missing
/// batches advance as event silence, accumulate exact tick lag, and request an
/// active-note flush so a lost note-off cannot become a stuck note.
///
/// Thread model:
///   * prepare()/release()                 — quiescent control thread; allocates.
///   * stage()/commit_through()            — one producer thread.
///   * pull()                              — one audio thread; allocation-free,
///                                           lock-free and wait-free.
///   * record_deadline_*()/stats()         — producer/control telemetry.

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/midi/ump.hpp>
#include <pulp/playback/clip_launch.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/timebase/tick.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::runtime {
class SpscRingIndex;
}

namespace pulp::playback {

using GeneratedEventCommitGeneration = std::uint64_t;
using GeneratedEventRevision = std::uint64_t;

/// A half-open span on the transport's non-looping clock. playback_epoch keeps
/// a seek or restart from aliasing an earlier continuous playback interval.
struct GeneratedEventSpan {
    std::uint64_t playback_epoch = 0;
    timebase::MonotonicBeat start{};
    timebase::MonotonicBeat end{};

    constexpr bool operator==(const GeneratedEventSpan&) const = default;
};

/// One value-owned generated event. offset is relative to its batch start and
/// must be non-negative, inside the half-open batch span, and nondecreasing.
struct GeneratedEvent {
    timebase::TickDuration offset{};
    midi::UmpPacket packet{};
};

enum class GeneratedEventDegradation : std::uint8_t {
    None,
    RepeatLastCommitted,
    UseSkeleton,
    Silence,
};

struct GeneratedEventDegradationLadder {
    static constexpr std::size_t maximum_steps = 3;
    std::array<GeneratedEventDegradation, maximum_steps> steps{
        GeneratedEventDegradation::RepeatLastCommitted,
        GeneratedEventDegradation::UseSkeleton,
        GeneratedEventDegradation::Silence,
    };
    std::uint8_t count = static_cast<std::uint8_t>(maximum_steps);

    std::span<const GeneratedEventDegradation> active() const noexcept {
        const auto active_count = count <= maximum_steps ? static_cast<std::size_t>(count) : 0;
        return {steps.data(), active_count};
    }
};

struct GeneratedEventSourceConfig {
    std::size_t committed_batch_capacity = 0;
    std::size_t staged_batch_capacity = 0;
    std::size_t maximum_events_per_batch = 0;
    /// Non-immediate grid whose exact boundaries may freeze staged batches.
    timeline::LaunchQuantize commit_quantize{};
    GeneratedEventDegradationLadder degradation{};
};

enum class GeneratedEventStageCode : std::uint8_t {
    Staged,
    Revised,
    NotPrepared,
    EpochNotStarted,
    WrongEpoch,
    InvalidSpan,
    NotQuantized,
    InvalidRevision,
    StaleRevision,
    FrozenSpan,
    OutOfOrder,
    OverlappingSpan,
    InvalidEvent,
    EventCapacityExceeded,
    StagingCapacityExceeded,
};

struct GeneratedEventStageResult {
    GeneratedEventStageCode code = GeneratedEventStageCode::NotPrepared;

    constexpr explicit operator bool() const noexcept {
        return code == GeneratedEventStageCode::Staged || code == GeneratedEventStageCode::Revised;
    }
};

enum class GeneratedEventCommitCode : std::uint8_t {
    Committed,
    NotPrepared,
    EpochNotStarted,
    WrongEpoch,
    InvalidBoundary,
    InvalidGeneration,
    StaleGeneration,
    CommitRingFull,
};

struct GeneratedEventCommitResult {
    GeneratedEventCommitCode code = GeneratedEventCommitCode::NotPrepared;
    std::size_t committed_batches = 0;

    constexpr explicit operator bool() const noexcept {
        return code == GeneratedEventCommitCode::Committed;
    }
};

enum class GeneratedEventPullCode : std::uint8_t {
    Ready,
    Starved,
    OutputOverflow,
    InvalidRequest,
};

struct GeneratedEventPullResult {
    GeneratedEventPullCode code = GeneratedEventPullCode::InvalidRequest;
    std::size_t event_count = 0;
    GeneratedEventCommitGeneration commit_generation = 0;
    std::uint64_t lagged_ticks = 0;
    /// Producer-selected fallback policy, or None before any declared miss.
    GeneratedEventDegradation degradation = GeneratedEventDegradation::None;
    /// The downstream renderer must emit its bounded all-notes-off/reset path.
    bool flush_active_notes = false;
};

class GeneratedEventSource {
  public:
    static constexpr audio::RtSafetyClass pull_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    GeneratedEventSource();
    ~GeneratedEventSource();

    GeneratedEventSource(const GeneratedEventSource&) = delete;
    GeneratedEventSource& operator=(const GeneratedEventSource&) = delete;

    /// Quiescent control thread. Allocates all staging, committed-ring and event
    /// storage. The quantization grid must be positive and the degradation
    /// ladder nonempty, duplicate-free and end in Silence.
    bool prepare(const GeneratedEventSourceConfig& config);

    /// Quiescent control thread. Starts a nonzero first playback epoch or a
    /// strictly newer seek/restart epoch, invalidating all pending work and
    /// live fallback selection. Preserves storage and cumulative telemetry.
    bool begin_playback_epoch(std::uint64_t playback_epoch) noexcept;

    /// Quiescent control thread. No producer or consumer may be active.
    void release() noexcept;

    /// Producer thread. Copies a new future batch, or replaces an exact staged
    /// span when revision is strictly newer. Published/frozen spans are never
    /// reachable from this path.
    GeneratedEventStageResult stage(GeneratedEventSpan span, GeneratedEventRevision revision,
                                    std::span<const GeneratedEvent> events) noexcept;

    /// Producer thread. Atomically publishes every staged batch ending at or
    /// before this exact quantized boundary. Generation is nonzero and strictly
    /// increasing, even for a commit that contains no event batches.
    GeneratedEventCommitResult commit_through(std::uint64_t playback_epoch,
                                              timebase::MonotonicBeat boundary,
                                              GeneratedEventCommitGeneration generation) noexcept;

    /// Audio thread. Consumes the exact requested span. A missing, stale or
    /// oversized batch advances the elapsed frontier rather than sliding late
    /// events into a later span.
    GeneratedEventPullResult pull(GeneratedEventSpan requested,
                                  std::span<GeneratedEvent> output) noexcept;

    /// Select or reset producer-declared fallback policy. Consuming an already
    /// committed batch does not imply that the producer met its deadline.
    /// Musical thinning remains producer-owned.
    GeneratedEventDegradation record_deadline_miss() noexcept;
    void record_deadline_met() noexcept;
    GeneratedEventDegradation current_degradation() const noexcept;

    bool prepared() const noexcept {
        return prepared_;
    }

    struct Stats {
        std::uint64_t committed_batches = 0;
        std::uint64_t consumed_batches = 0;
        std::uint64_t starvation_events = 0;
        std::uint64_t lagged_ticks = 0;
        std::uint64_t late_batches = 0;
        std::uint64_t output_overflows = 0;
        std::uint64_t deadline_misses = 0;
        std::array<std::uint64_t, GeneratedEventDegradationLadder::maximum_steps>
            degradation_uses{};
        std::size_t committed_batches_ready = 0;
    };

    Stats stats() const noexcept;

  private:
    struct BatchSlot {
        GeneratedEventSpan span{};
        GeneratedEventRevision revision = 0;
        GeneratedEventCommitGeneration commit_generation = 0;
        std::size_t event_count = 0;
    };

    struct ElapsedFrontierSnapshot {
        std::uint64_t epoch = 0;
        std::int64_t tick = 0;
        bool present = false;
    };

    bool valid_span(const GeneratedEventSpan& span) const noexcept;
    bool is_quantized(timebase::MonotonicBeat boundary) const noexcept;
    bool valid_events(const GeneratedEventSpan& span,
                      std::span<const GeneratedEvent> events) const noexcept;
    void publish_elapsed(const GeneratedEventSpan& span) noexcept;
    bool elapsed_frontier(std::uint64_t& epoch, std::int64_t& tick) const noexcept;
    GeneratedEvent* staged_event_storage(std::size_t index) noexcept;
    GeneratedEvent* committed_event_storage(std::size_t index) noexcept;

    GeneratedEventSourceConfig config_{};
    std::vector<BatchSlot> staged_{};
    std::vector<GeneratedEvent> staged_events_{};
    std::size_t staged_count_ = 0;
    std::vector<BatchSlot> committed_{};
    std::vector<GeneratedEvent> committed_events_{};
    std::unique_ptr<runtime::SpscRingIndex> committed_ring_{};

    std::uint64_t committed_epoch_ = 0;
    timebase::MonotonicBeat committed_through_{};
    GeneratedEventCommitGeneration last_commit_generation_ = 0;
    std::uint64_t active_epoch_ = 0;
    bool has_committed_frontier_ = false;
    bool has_active_epoch_ = false;
    bool prepared_ = false;
    std::atomic<std::uint8_t> consecutive_deadline_misses_{0};

    // Audio-thread-owned mirror prevents an invalid pull from regressing the
    // atomically published producer frontier.
    std::uint64_t consumer_elapsed_epoch_ = 0;
    std::int64_t consumer_elapsed_tick_ = 0;
    bool has_consumer_elapsed_frontier_ = false;

    // One audio writer, one producer reader. Use the shared primitive so its
    // ARM-safe read fence and coherence tests govern this frontier too.
    runtime::SeqLock<ElapsedFrontierSnapshot> elapsed_frontier_{};
    std::atomic<std::uint8_t> current_degradation_index_{
        static_cast<std::uint8_t>(GeneratedEventDegradationLadder::maximum_steps)};
    std::atomic<std::uint64_t> committed_batches_{0};
    std::atomic<std::uint64_t> consumed_batches_{0};
    std::atomic<std::uint64_t> starvation_events_{0};
    std::atomic<std::uint64_t> lagged_ticks_{0};
    std::atomic<std::uint64_t> late_batches_{0};
    std::atomic<std::uint64_t> output_overflows_{0};
    std::atomic<std::uint64_t> deadline_misses_{0};
    std::array<std::atomic<std::uint64_t>, GeneratedEventDegradationLadder::maximum_steps>
        degradation_uses_{};
};

} // namespace pulp::playback
