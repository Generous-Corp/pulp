#include <pulp/timeline/compile_context.hpp>

namespace pulp::timeline {

CompileContextView::CompileContextView(const Project& project, ItemId sequence_id,
                                       CompileContextSubscriptions subscriptions) noexcept
    : sequence_(project.find_sequence(sequence_id)), subscriptions_(subscriptions) {}

const ChordScaleLane* CompileContextView::chord_scale_lane() const noexcept {
    if (!sequence_ || !subscriptions_.reads(CompileContextKind::ChordScale))
        return nullptr;
    return &sequence_->chord_scale_lane();
}

const ChordScaleEvent*
CompileContextView::chord_scale_at(timebase::TickPosition position) const noexcept {
    const auto* lane = chord_scale_lane();
    return lane ? lane->at(position) : nullptr;
}

const GrooveTemplate* CompileContextView::groove() const noexcept {
    if (!sequence_ || !subscriptions_.reads(CompileContextKind::Groove))
        return nullptr;
    return &sequence_->groove();
}

} // namespace pulp::timeline
