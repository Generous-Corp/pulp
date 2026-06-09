#include <pulp/audio/sample_edit_document.hpp>

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace pulp::audio {
namespace {

bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

bool finite_positive(float value) noexcept {
    return std::isfinite(value) && value > 0.0f;
}

bool same_source(const EditableSampleSource& a, const EditableSampleSource& b) noexcept {
    return a.sample_id == b.sample_id &&
           a.generation == b.generation &&
           a.num_channels == b.num_channels &&
           a.num_frames == b.num_frames &&
           a.sample_rate == b.sample_rate;
}

bool valid_range(std::uint64_t start, std::uint64_t end, std::uint64_t limit) noexcept {
    return start <= end && end <= limit;
}

bool same_trim(const EditableSampleState& a, const EditableSampleState& b) noexcept {
    return a.trim_start == b.trim_start && a.trim_end == b.trim_end;
}

bool same_fades(const EditableSampleState& a, const EditableSampleState& b) noexcept {
    return a.fade_in_frames == b.fade_in_frames && a.fade_out_frames == b.fade_out_frames;
}

bool same_loop(const EditableSampleState& a, const EditableSampleState& b) noexcept {
    return a.has_loop == b.has_loop && a.loop_start == b.loop_start && a.loop_end == b.loop_end;
}

bool loop_cleared(const EditableSampleState& state) noexcept {
    return !state.has_loop && state.loop_start == 0 && state.loop_end == 0;
}

bool same_reverse(const EditableSampleState& a, const EditableSampleState& b) noexcept {
    return a.reversed == b.reversed;
}

bool same_gain(const EditableSampleState& a, const EditableSampleState& b) noexcept {
    return a.normalize_gain == b.normalize_gain;
}

bool kind_delta_valid(SampleEditOperationKind kind,
                      const EditableSampleState& before,
                      const EditableSampleState& after) noexcept {
    switch (kind) {
        case SampleEditOperationKind::set_trim:
            return same_fades(before, after) &&
                   same_reverse(before, after) &&
                   same_gain(before, after) &&
                   (same_loop(before, after) || loop_cleared(after));
        case SampleEditOperationKind::set_fades:
            return same_trim(before, after) &&
                   same_loop(before, after) &&
                   same_reverse(before, after) &&
                   same_gain(before, after);
        case SampleEditOperationKind::set_loop:
            return same_trim(before, after) &&
                   same_fades(before, after) &&
                   after.has_loop &&
                   same_reverse(before, after) &&
                   same_gain(before, after);
        case SampleEditOperationKind::clear_loop:
            return same_trim(before, after) &&
                   same_fades(before, after) &&
                   loop_cleared(after) &&
                   same_reverse(before, after) &&
                   same_gain(before, after);
        case SampleEditOperationKind::set_reverse:
            return same_trim(before, after) &&
                   same_fades(before, after) &&
                   same_loop(before, after) &&
                   same_gain(before, after);
        case SampleEditOperationKind::set_normalize_gain:
            return same_trim(before, after) &&
                   same_fades(before, after) &&
                   same_loop(before, after) &&
                   same_reverse(before, after);
        case SampleEditOperationKind::none:
            return false;
    }
    return false;
}

}  // namespace

bool EditableSampleSource::valid() const noexcept {
    return sample_id != kInvalidSampleId &&
           generation > 0 &&
           num_channels > 0 &&
           num_frames > 0 &&
           finite_positive(sample_rate);
}

EditableSampleSource EditableSampleSource::from_published_sample(std::uint32_t sample_id,
                                                                 const PublishedSampleView& view) noexcept {
    if (!view.valid) return {};
    return {sample_id, view.generation, view.num_channels, view.num_frames, view.sample_rate};
}

bool EditableSampleState::valid() const noexcept {
    if (!source.valid()) return false;
    if (!valid_range(trim_start, trim_end, source.num_frames)) return false;
    if (trim_start == trim_end) return false;
    if (fade_in_frames > trimmed_frames()) return false;
    if (fade_out_frames > trimmed_frames()) return false;
    if (!finite_positive(normalize_gain)) return false;
    if (has_loop && !valid_range(loop_start, loop_end, source.num_frames)) return false;
    if (has_loop && loop_start == loop_end) return false;
    if (has_loop && (loop_start < trim_start || loop_end > trim_end)) return false;
    return true;
}

std::uint64_t EditableSampleState::trimmed_frames() const noexcept {
    return trim_end >= trim_start ? trim_end - trim_start : 0;
}

bool SampleEditOperation::valid() const noexcept {
    if (kind == SampleEditOperationKind::none) return false;
    if (!before.valid() || !after.valid()) return false;
    if (!same_source(before.source, after.source)) return false;
    if (after.revision != before.revision + 1) return false;
    return kind_delta_valid(kind, before, after);
}

bool EditableSampleDocument::reset(EditableSampleSource source) noexcept {
    if (!source.valid()) {
        clear();
        return false;
    }

    state_ = {};
    state_.source = source;
    state_.revision = 1;
    state_.trim_start = 0;
    state_.trim_end = source.num_frames;
    state_.normalize_gain = 1.0f;
    return true;
}

void EditableSampleDocument::clear() noexcept {
    state_ = {};
}

SampleEditOperation EditableSampleDocument::make_set_trim(std::uint64_t start,
                                                          std::uint64_t end) const noexcept {
    auto after = state_;
    after.trim_start = start;
    after.trim_end = end;
    if (after.has_loop && (after.loop_start < start || after.loop_end > end)) {
        after.has_loop = false;
        after.loop_start = 0;
        after.loop_end = 0;
    }
    return make_operation(SampleEditOperationKind::set_trim, after);
}

SampleEditOperation EditableSampleDocument::make_set_fades(std::uint64_t fade_in_frames,
                                                           std::uint64_t fade_out_frames) const noexcept {
    auto after = state_;
    after.fade_in_frames = fade_in_frames;
    after.fade_out_frames = fade_out_frames;
    return make_operation(SampleEditOperationKind::set_fades, after);
}

SampleEditOperation EditableSampleDocument::make_set_loop(std::uint64_t start,
                                                          std::uint64_t end) const noexcept {
    auto after = state_;
    after.has_loop = true;
    after.loop_start = start;
    after.loop_end = end;
    return make_operation(SampleEditOperationKind::set_loop, after);
}

SampleEditOperation EditableSampleDocument::make_clear_loop() const noexcept {
    auto after = state_;
    after.has_loop = false;
    after.loop_start = 0;
    after.loop_end = 0;
    return make_operation(SampleEditOperationKind::clear_loop, after);
}

SampleEditOperation EditableSampleDocument::make_set_reverse(bool reversed) const noexcept {
    auto after = state_;
    after.reversed = reversed;
    return make_operation(SampleEditOperationKind::set_reverse, after);
}

SampleEditOperation EditableSampleDocument::make_set_normalize_gain(float gain) const noexcept {
    auto after = state_;
    after.normalize_gain = gain;
    return make_operation(SampleEditOperationKind::set_normalize_gain, after);
}

bool EditableSampleDocument::apply(const SampleEditOperation& operation) noexcept {
    if (!operation.valid()) return false;
    if (!state_.valid()) return false;
    if (!same_source(state_.source, operation.before.source)) return false;
    if (state_.revision != operation.before.revision) return false;

    state_ = operation.after;
    return state_.valid();
}

bool EditableSampleDocument::restore_state(const EditableSampleState& state) noexcept {
    if (!state.valid()) return false;
    state_ = state;
    return true;
}

SampleEditOperation EditableSampleDocument::make_operation(SampleEditOperationKind kind,
                                                          EditableSampleState after) const noexcept {
    SampleEditOperation operation;
    operation.kind = kind;
    operation.before = state_;
    operation.after = after;
    operation.after.revision = state_.revision + 1;
    if (!operation.valid()) {
        return {};
    }
    return operation;
}

bool SampleEditHistory::prepare(std::size_t max_operations) {
    std::vector<SampleEditOperation> next_undo;
    std::vector<SampleEditOperation> next_redo;
    try {
        next_undo.reserve(max_operations);
        next_redo.reserve(max_operations);
    } catch (const std::bad_alloc&) {
        return false;
    }

    capacity_ = max_operations;
    undo_stack_ = std::move(next_undo);
    redo_stack_ = std::move(next_redo);
    return true;
}

void SampleEditHistory::clear() noexcept {
    undo_stack_.clear();
    redo_stack_.clear();
}

bool SampleEditHistory::perform(EditableSampleDocument& document,
                                const SampleEditOperation& operation) {
    if (capacity_ == 0 || undo_stack_.size() >= capacity_) return false;
    if (!operation.valid()) return false;
    if (!document.apply(operation)) return false;

    undo_stack_.push_back(operation);
    redo_stack_.clear();
    return true;
}

bool SampleEditHistory::undo(EditableSampleDocument& document) {
    if (undo_stack_.empty() || redo_stack_.size() >= capacity_) return false;

    const auto operation = undo_stack_.back();
    if (!document.restore_state(operation.before)) return false;

    undo_stack_.pop_back();
    redo_stack_.push_back(operation);
    return true;
}

bool SampleEditHistory::redo(EditableSampleDocument& document) {
    if (redo_stack_.empty() || undo_stack_.size() >= capacity_) return false;

    const auto operation = redo_stack_.back();
    if (!document.apply(operation)) return false;

    redo_stack_.pop_back();
    undo_stack_.push_back(operation);
    return true;
}

}  // namespace pulp::audio
