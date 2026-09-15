#pragma once // Private playback implementation detail.

#include "placement_fade.hpp"

#include <pulp/playback/program_compiler.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pulp::playback {

struct SequenceLoweringError {
    CompileErrorCode code = CompileErrorCode::InvalidStructure;
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
    // Every enclosing placement's fade, in this track's tick coordinates, from
    // outermost to innermost. Empty for a clip no faded placement encloses,
    // which is every clip that was not nested and most that were.
    //
    // A fade does not fold into the leaf the way gain does. Gain composes into
    // one scalar the leaf can carry in its own `gain_linear`; a fade is
    // time-varying, so a leaf covering part of one needs the ramp itself and
    // its own position within it, and two ramps of different shapes do not
    // reduce to a third. Hence a list travelling beside the clip rather than
    // extra fields inside it.
    std::vector<LoweredPlacementFade> placement_fades;
    // The authored clip this one is a window onto. A nesting can trim a leaf,
    // and content whose renderer generates from the authored origin has to be
    // generated over that origin or its pattern phase moves with the trim, so
    // the window travels beside the clip and the compiler applies it to what
    // the renderer returned rather than asking the renderer to apply it.
    //
    // `authored_window_start` is the tick inside the authored clip at which the
    // retained window begins, and `authored_duration` is the authored clip's
    // own duration. An untrimmed leaf reads zero and its own duration, which
    // windows to exactly the clip it lowered to before.
    std::int64_t authored_window_start = 0;
    timebase::TickDuration authored_duration{0};
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
