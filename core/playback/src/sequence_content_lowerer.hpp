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
    // The sequence context read by this leaf. Invalid means the compile root.
    timeline::ItemId context_sequence_id;
    // Owner-sequence tick corresponding to `clip.start()` after flattening.
    timebase::TickPosition context_start;
    // Ticks of note content retained OUTSIDE this clip's audible window, so a
    // groove that pulls a note across a trim edge still has that note to pull.
    //
    // Note offsets on a padded clip are measured from
    // `clip.start() - groove_pad_left`, not from `clip.start()`. `context_start`
    // deliberately keeps naming the owner tick of `clip.start()` itself, because
    // registered content and chord lookups anchor on it and must not move.
    //
    // Both are zero unless the leaf was trimmed by its nesting AND its owner's
    // groove actually displaces, so an untrimmed or feel-free leaf lowers to the
    // same clip it always did. Only note content is padded: automation lanes are
    // not groove-displaced, so they stay anchored to the unpadded window.
    std::int64_t groove_pad_left = 0;
    std::int64_t groove_pad_right = 0;
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
