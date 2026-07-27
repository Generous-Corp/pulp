#pragma once // Private playback implementation detail.

#include "sequence_content_lowerer.hpp"

#include <pulp/playback/program.hpp>

#include <cstdint>
#include <optional>

namespace pulp::playback::detail {

/// Owns the generated-identity and expansion accounting that permits immutable
/// track programs to be reused without changing later tracks' generated IDs.
class SequenceCompileBookkeeping {
  public:
    void begin_track(const SequenceContentLowerer& lowerer) noexcept;
    void finish_flattened_track(const SequenceContentLowerer& lowerer) noexcept;

    bool generated_layout_matches(const TrackProgram& prior,
                                  const SequenceContentLowerer& lowerer) const noexcept;
    std::optional<SequenceLoweringError> charge_reused(const TrackProgram& prior,
                                                       SequenceContentLowerer& lowerer,
                                                       timeline::ItemId track_id) const;

    std::uint64_t expanded_clip_count() const noexcept {
        return current_expanded_clip_count_;
    }
    std::uint64_t expanded_note_event_count() const noexcept {
        return current_expanded_note_event_count_;
    }
    std::uint64_t generated_id_start() const noexcept {
        return generated_id_start_;
    }
    std::uint64_t generated_id_count() const noexcept {
        return current_generated_id_count_;
    }

    void reset_current() noexcept;

  private:
    std::uint64_t expanded_clip_start_ = 0;
    std::uint64_t expanded_note_event_start_ = 0;
    std::uint64_t generated_id_start_ = 0;
    std::uint64_t current_expanded_clip_count_ = 0;
    std::uint64_t current_expanded_note_event_count_ = 0;
    std::uint64_t current_generated_id_count_ = 0;
};

} // namespace pulp::playback::detail
