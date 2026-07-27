#include "sequence_compile_bookkeeping.hpp"

namespace pulp::playback::detail {

void SequenceCompileBookkeeping::begin_track(const SequenceContentLowerer& lowerer) noexcept {
    generated_id_start_ = lowerer.next_generated_id();
    expanded_clip_start_ = lowerer.expanded_clip_count();
    expanded_note_event_start_ = lowerer.expanded_note_event_count();
}

void SequenceCompileBookkeeping::finish_flattened_track(
    const SequenceContentLowerer& lowerer) noexcept {
    current_expanded_clip_count_ = lowerer.expanded_clip_count() - expanded_clip_start_;
    current_expanded_note_event_count_ =
        lowerer.expanded_note_event_count() - expanded_note_event_start_;
    current_generated_id_count_ = lowerer.next_generated_id() - generated_id_start_;
}

bool SequenceCompileBookkeeping::generated_layout_matches(
    const TrackProgram& prior, const SequenceContentLowerer& lowerer) const noexcept {
    return prior.generated_id_start() == lowerer.next_generated_id();
}

std::optional<SequenceLoweringError> SequenceCompileBookkeeping::charge_reused(
    const TrackProgram& prior, SequenceContentLowerer& lowerer, timeline::ItemId track_id) const {
    return lowerer.charge_reused(prior.expanded_clip_count(), prior.expanded_note_event_count(),
                                 prior.generated_id_count(), track_id);
}

void SequenceCompileBookkeeping::reset_current() noexcept {
    expanded_clip_start_ = 0;
    expanded_note_event_start_ = 0;
    generated_id_start_ = 0;
    current_expanded_clip_count_ = 0;
    current_expanded_note_event_count_ = 0;
    current_generated_id_count_ = 0;
}

} // namespace pulp::playback::detail
