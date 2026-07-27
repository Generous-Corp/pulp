#pragma once // Private playback implementation detail.

#include <pulp/playback/program_compiler.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pulp::playback {

struct SequenceLoweringError {
    CompileErrorCode code = CompileErrorCode::NestedSequenceUnsupported;
    timeline::ItemId item;
};

enum class TrackContentKind : std::uint8_t {
    Arrangement,
    ActiveTake,
    Freeze,
};

struct TrackContentPlan {
    TrackContentKind kind = TrackContentKind::Arrangement;
    const timeline::TakeLane* active_take_lane = nullptr;
    const timeline::TrackFreeze* freeze = nullptr;
    std::uint64_t source_count = 0;
};

struct LoweredClip {
    timeline::Clip clip;
    double source_frame_offset = 0.0;
};

class SequenceContentLowerer {
  public:
    struct StepResult {
        bool complete = false;
        std::optional<SequenceLoweringError> error;
    };

    struct BeginTrackResult {
        TrackContentPlan plan;
        std::optional<SequenceLoweringError> error;
    };

    SequenceContentLowerer(const timeline::Project& project,
                           const timebase::CompiledTempoMap& tempo_map,
                           std::uint64_t max_expanded_note_events,
                           std::uint64_t max_expanded_clips);
    ~SequenceContentLowerer();

    SequenceContentLowerer(const SequenceContentLowerer&) = delete;
    SequenceContentLowerer& operator=(const SequenceContentLowerer&) = delete;
    SequenceContentLowerer(SequenceContentLowerer&&) noexcept;
    SequenceContentLowerer& operator=(SequenceContentLowerer&&) noexcept;

    BeginTrackResult begin_track(const timeline::Track& track, std::vector<LoweredClip>& output);
    std::optional<SequenceLoweringError> charge_reused(std::uint64_t clips,
                                                       std::uint64_t note_events,
                                                       std::uint64_t generated_ids,
                                                       timeline::ItemId track_id);
    std::uint64_t expanded_clip_count() const noexcept;
    std::uint64_t expanded_note_event_count() const noexcept;
    std::uint64_t next_generated_id() const noexcept;
    StepResult step();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::playback
