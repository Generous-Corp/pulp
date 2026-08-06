#include "sequence_content_lowerer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::playback {

class SequenceContentLowerer::Impl {
  public:
    Impl(const timeline::Project& project, const timebase::CompiledTempoMap& tempo_map,
         std::uint64_t max_expanded_note_events, std::uint64_t max_expanded_clips)
        : project_(project), tempo_map_(tempo_map),
          max_expanded_note_events_(max_expanded_note_events),
          max_expanded_clips_(max_expanded_clips), next_generated_id_(project.next_item_id()) {}

    BeginTrackResult begin_track(const timeline::Track& track, std::vector<LoweredClip>& output) {
        if (track.freeze()) {
            output.clear();
            if (const auto error = charge(1, 0, 0, track.id()))
                return {{}, error};
            return {
                {.kind = TrackContentKind::Freeze, .freeze = &*track.freeze(), .source_count = 1},
                std::nullopt};
        }
        if (track.active_take_lane_id().valid()) {
            const auto* lane = track.find_take_lane(track.active_take_lane_id());
            if (!lane)
                return {{},
                        SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                              track.active_take_lane_id()}};
            const auto count = lane->comp_segments().size();
            output.clear();
            if (const auto error = charge(count, 0, 0, track.id()))
                return {{}, error};
            return {{.kind = TrackContentKind::ActiveTake,
                     .active_take_lane = lane,
                     .source_count = count},
                    std::nullopt};
        }
        reset_arrangement(track, output);
        return {{.kind = TrackContentKind::Arrangement}, std::nullopt};
    }

    void reset_arrangement(const timeline::Track& track, std::vector<LoweredClip>& output) {
        root_ = &track;
        output_ = &output;
        root_clip_index_ = 0;
        frames_.clear();
        pending_leaf_.reset();
        output.clear();
        const auto remaining = max_expanded_clips_ - std::min(expanded_clips_, max_expanded_clips_);
        const auto capacity =
            static_cast<std::size_t>(std::min<std::uint64_t>(track.clips().size(), remaining));
        output.reserve(capacity);
    }

    std::optional<SequenceLoweringError> charge_reused(std::uint64_t clips,
                                                       std::uint64_t note_events,
                                                       std::uint64_t generated_ids,
                                                       timeline::ItemId track_id) {
        return charge(clips, note_events, generated_ids, track_id);
    }

    std::optional<SequenceLoweringError> charge(std::uint64_t clips, std::uint64_t note_events,
                                                std::uint64_t generated_ids,
                                                timeline::ItemId track_id) {
        if (clips > max_expanded_clips_ - std::min(expanded_clips_, max_expanded_clips_) ||
            note_events > max_expanded_note_events_ -
                              std::min(expanded_note_events_, max_expanded_note_events_) ||
            generated_ids > std::numeric_limits<std::uint64_t>::max() - next_generated_id_)
            return SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded, track_id};
        expanded_clips_ += clips;
        expanded_note_events_ += note_events;
        next_generated_id_ += generated_ids;
        return std::nullopt;
    }

    std::uint64_t expanded_clip_count() const noexcept {
        return expanded_clips_;
    }

    std::uint64_t expanded_note_event_count() const noexcept {
        return expanded_note_events_;
    }

    std::uint64_t next_generated_id() const noexcept {
        return next_generated_id_;
    }

    StepResult step() {
        if (pending_leaf_)
            return step_pending_leaf();
        if (!frames_.empty())
            return step_reference();
        if (root_clip_index_ == root_->clips().size())
            return {.complete = true};

        const auto& clip = root_->clips()[root_clip_index_++];
        const auto* reference = std::get_if<timeline::SequenceRef>(&clip.content());
        if (!reference)
            return append(clip, clip.id());

        if (const auto error = validate_reference(clip, *reference, 1))
            return {.error = error};
        // Retain the document-owned placement identity in the lowered plan;
        // its nested payload becomes generated leaf clips.
        auto sentinel =
            timeline::Clip::create(clip.id(), clip.start(), clip.duration(),
                                   timeline::EmptyContent{}, clip.playback_properties());
        if (!sentinel)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure, clip.id()}};
        auto appended = append(std::move(sentinel).value(), clip.id());
        if (appended.error)
            return appended;
        push_reference(clip, *reference, 1);
        return {};
    }

  private:
    struct ReferenceFrame {
        timeline::Clip placement;
        timeline::SequenceRef reference;
        const timeline::Sequence* sequence = nullptr;
        timebase::TickPosition source_end;
        std::size_t depth = 0;
        std::size_t track_index = 0;
        std::size_t clip_index = 0;
    };

    struct PendingLeaf {
        timeline::Clip child;
        timeline::ItemId generated_id;
        timebase::TickPosition clipped_start;
        timebase::TickPosition clipped_end;
        timebase::TickPosition target_start;
        timebase::TickDuration target_duration;
        std::int64_t left_trim = 0;
        std::int64_t right_trim = 0;
        std::size_t note_index = 0;
        std::vector<timeline::NoteEvent> clipped_notes;
        timeline::ItemId context_sequence_id;
    };

    StepResult append(timeline::Clip clip, timeline::ItemId source,
                      double source_frame_offset = 0.0, timeline::ItemId context_sequence_id = {},
                      std::optional<timebase::TickPosition> context_start = std::nullopt) {
        if (expanded_clips_ >= max_expanded_clips_)
            return {.error =
                        SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded, source}};
        ++expanded_clips_;
        const auto authored_start = context_start.value_or(clip.start());
        output_->push_back(
            {std::move(clip), source_frame_offset, context_sequence_id, authored_start});
        return {};
    }

    StepResult charge_reference(timeline::ItemId source) {
        if (expanded_clips_ >= max_expanded_clips_)
            return {.error =
                        SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded, source}};
        ++expanded_clips_;
        return {};
    }

    std::optional<SequenceLoweringError> validate_reference(const timeline::Clip& placement,
                                                            const timeline::SequenceRef& reference,
                                                            std::size_t depth) const {
        if (depth > timeline::kMaxSequenceNestingDepth ||
            placement.time_anchor() != timeline::ClipTimeAnchor::Musical)
            return SequenceLoweringError{CompileErrorCode::NestedSequenceUnsupported,
                                         placement.id()};
        const auto playback = placement.playback_properties();
        if (playback.gain_linear != 1.0f || playback.fade_in_duration != 0 ||
            playback.fade_out_duration != 0)
            return SequenceLoweringError{CompileErrorCode::NestedSequenceUnsupported,
                                         placement.id()};
        if (!project_.find_sequence(reference.sequence_id))
            return SequenceLoweringError{CompileErrorCode::InvalidStructure, reference.sequence_id};
        if (reference.source_start.value >
            std::numeric_limits<std::int64_t>::max() - placement.duration().value)
            return SequenceLoweringError{CompileErrorCode::InvalidStructure, placement.id()};
        return std::nullopt;
    }

    void push_reference(const timeline::Clip& placement, const timeline::SequenceRef& reference,
                        std::size_t depth) {
        frames_.push_back(
            {placement, reference, project_.find_sequence(reference.sequence_id),
             reference.source_start + timebase::TickDuration{placement.duration().value}, depth});
    }

    StepResult step_reference() {
        auto& frame = frames_.back();
        if (frame.track_index == frame.sequence->tracks().size()) {
            frames_.pop_back();
            return {};
        }
        const auto& track = frame.sequence->tracks()[frame.track_index];
        if (frame.clip_index == 0 &&
            (!track.device_chain().empty() || !track.automation_lanes().empty() ||
             !track.take_lanes().empty() || track.freeze() || track.active_take_lane_id().valid() ||
             track.record_armed() ||
             // Flattening a nested track folds it into the parent, which has no
             // place to put a child fader: the child's gain and pan would simply
             // stop applying. Refuse rather than silently play the child at
             // unity — the same choice the clip-level guard above makes for clip
             // gain and fades. Composing mixer state through nesting is a
             // separate feature, not a default.
             !(track.mixer() == timeline::TrackMixer{})))
            return {.error = SequenceLoweringError{CompileErrorCode::NestedSequenceUnsupported,
                                                   track.id()}};
        if (frame.clip_index == track.clips().size()) {
            ++frame.track_index;
            frame.clip_index = 0;
            return {};
        }

        const auto& child = track.clips()[frame.clip_index++];
        if (auto charged = charge_reference(child.id()); charged.error)
            return charged;
        if (child.time_anchor() != timeline::ClipTimeAnchor::Musical)
            return {.error = SequenceLoweringError{CompileErrorCode::NestedSequenceUnsupported,
                                                   child.id()}};
        const auto clipped_start = std::max(child.start(), frame.reference.source_start);
        const auto clipped_end = std::min(child.end(), frame.source_end);
        if (clipped_end <= clipped_start)
            return {};
        const auto relative_start =
            timebase::TickDuration{clipped_start.value - frame.reference.source_start.value};
        if (frame.placement.start().value >
            std::numeric_limits<std::int64_t>::max() - relative_start.value)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure, child.id()}};
        const auto target_start = frame.placement.start() + relative_start;
        const auto target_duration =
            timebase::TickDuration{clipped_end.value - clipped_start.value};
        const auto left_trim = clipped_start.value - child.start().value;
        const auto right_trim = child.end().value - clipped_end.value;
        if (std::holds_alternative<timeline::MediaRef>(child.content())) {
            // A conforming clip maps its complete authored source span onto its
            // musical placement. The legacy nested-trim path below advances a
            // raw source-frame offset from elapsed timeline samples, which is
            // only valid for TimeConform::None. Refuse a partial view until the
            // renderer owns a conform-aware source-range mapping; otherwise a
            // nested tempo-ramped clip can silently start at the wrong audio.
            if (child.time_conform() != timeline::TimeConform::None &&
                (left_trim != 0 || right_trim != 0))
                return {.error = SequenceLoweringError{
                            CompileErrorCode::NestedSequenceUnsupported, child.id()}};
            const auto playback = child.playback_properties();
            const auto retained_start = static_cast<std::uint64_t>(left_trim);
            const auto retained_end =
                static_cast<std::uint64_t>(child.duration().value - right_trim);
            const auto child_duration = static_cast<std::uint64_t>(child.duration().value);
            const bool cuts_fade_in =
                playback.fade_in_duration > 0 && retained_start < playback.fade_in_duration &&
                !(retained_start == 0 && retained_end >= playback.fade_in_duration);
            const auto fade_out_start = child_duration - playback.fade_out_duration;
            const bool cuts_fade_out =
                playback.fade_out_duration > 0 && retained_end > fade_out_start &&
                !(retained_end == child_duration && retained_start <= fade_out_start);
            if (cuts_fade_in || cuts_fade_out)
                return {.error = SequenceLoweringError{CompileErrorCode::NestedSequenceUnsupported,
                                                       child.id()}};
        }

        if (const auto* nested = std::get_if<timeline::SequenceRef>(&child.content())) {
            if (nested->source_start.value >
                std::numeric_limits<std::int64_t>::max() - left_trim)
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, child.id()}};
            auto nested_clip = timeline::Clip::create(
                child.id(), target_start, target_duration,
                timeline::SequenceRef{nested->sequence_id,
                                      nested->source_start + timebase::TickDuration{left_trim}},
                child.playback_properties());
            if (!nested_clip)
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, child.id()}};
            const auto reference = std::get<timeline::SequenceRef>(nested_clip->content());
            const auto depth = frame.depth + 1;
            if (const auto error = validate_reference(nested_clip.value(), reference, depth))
                return {.error = error};
            if (auto charged = charge_reference(child.id()); charged.error)
                return charged;
            push_reference(nested_clip.value(), reference, depth);
            return {};
        }

        if (next_generated_id_ == 0 ||
            next_generated_id_ == std::numeric_limits<std::uint64_t>::max())
            return {.error = SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded,
                                                   child.id()}};
        pending_leaf_ = PendingLeaf{
            child,
            timeline::ItemId{next_generated_id_++},
            clipped_start,
            clipped_end,
            target_start,
            target_duration,
            left_trim,
            right_trim,
            0,
            {},
            frame.sequence->id(),
        };
        if (!std::holds_alternative<timeline::MidiContent>(child.content()))
            return finish_pending_leaf();
        return {};
    }

    StepResult step_pending_leaf() {
        auto& pending = *pending_leaf_;
        const auto& notes = std::get<timeline::MidiContent>(pending.child.content()).notes();
        if (pending.note_index == notes.size())
            return finish_pending_leaf();
        const auto& note = notes[pending.note_index++];
        if (inspected_note_events_ > max_expanded_note_events_ ||
            max_expanded_note_events_ - inspected_note_events_ < 2)
            return {.error = SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded,
                                                   pending.child.id()}};
        inspected_note_events_ += 2;
        if (note.start.value < 0 || note.start.value > pending.child.duration().value ||
            note.duration.value > pending.child.duration().value - note.start.value)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure, note.id}};
        if (pending.child.start().value >
            std::numeric_limits<std::int64_t>::max() - note.start.value)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure, note.id}};
        const auto note_start = pending.child.start().value + note.start.value;
        if (note_start >
            std::numeric_limits<std::int64_t>::max() - note.duration.value)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure, note.id}};
        const auto note_end = note_start + note.duration.value;
        const auto audible_start = std::max(note_start, pending.clipped_start.value);
        const auto audible_end = std::min(note_end, pending.clipped_end.value);
        if (audible_end <= audible_start)
            return {};
        if (expanded_note_events_ > max_expanded_note_events_ ||
            max_expanded_note_events_ - expanded_note_events_ < 2)
            return {.error = SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded,
                                                   pending.child.id()}};
        expanded_note_events_ += 2;
        auto clipped_note = note;
        clipped_note.start = timebase::TickPosition{audible_start - pending.clipped_start.value};
        clipped_note.duration = timebase::TickDuration{audible_end - audible_start};
        pending.clipped_notes.push_back(clipped_note);
        return {};
    }

    StepResult finish_pending_leaf() {
        auto pending = std::move(*pending_leaf_);
        pending_leaf_.reset();
        timeline::ClipContent content = pending.child.content();
        double source_frame_offset = 0.0;
        if (const auto* notes = std::get_if<timeline::MidiContent>(&content)) {
            const auto* owner = project_.find_sequence(pending.context_sequence_id);
            if (!owner)
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.context_sequence_id}};
            if ((pending.left_trim != 0 || pending.right_trim != 0) &&
                !owner->groove().states_no_feel())
                return {.error = SequenceLoweringError{CompileErrorCode::TrimmedGrooveUnsupported,
                                                       pending.child.id()}};
            // A nested note clip keeps its modifiers and its authored seed.
            // Rebuilding with the notes alone would leave the notes sounding
            // unconditionally inside a SequenceRef while they honour their
            // probability / condition / ratchet everywhere else — a silent
            // difference, because the notes-only overload still compiles.
            //
            // Clipping drops notes that fall entirely outside the audible
            // window, and a modifier must name a note that is still present, so
            // the companion array is filtered to the retained ids rather than
            // passed through. The seed is carried verbatim: it selects the
            // replay, so changing it would change which notes sound.
            const auto retained = [&](timeline::ItemId note_id) {
                return std::any_of(pending.clipped_notes.begin(), pending.clipped_notes.end(),
                                   [&](const timeline::NoteEvent& note) {
                                       return note.id == note_id;
                                   });
            };
            std::vector<timeline::NoteModifier> modifiers;
            for (const auto& modifier : notes->modifiers())
                if (retained(modifier.note_id))
                    modifiers.push_back(modifier);
            const auto seed = notes->modifier_seed();
            // Trimming a nested clip has no defined answer for a controller
            // lane yet: a point before the retained window can still be the
            // value that is sounding inside it, so neither dropping nor
            // carrying it is correct. The named refusal says which decision is
            // missing; silently rebuilding without the lanes would lower a clip
            // whose controllers stopped existing.
            if (!notes->lanes().empty())
                return {.error =
                            SequenceLoweringError{CompileErrorCode::TrimmedMidiLaneUnsupported,
                                                  pending.child.id()}};
            auto rebuilt = timeline::MidiContent::create(std::move(pending.clipped_notes),
                                                         std::move(modifiers), seed);
            if (!rebuilt)
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.child.id()}};
            content = std::move(rebuilt).value();
        } else if (auto* media = std::get_if<timeline::MediaRef>(&content);
                   media && pending.left_trim > 0) {
            if (pending.target_start.value <
                std::numeric_limits<std::int64_t>::min() + pending.left_trim)
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.child.id()}};
            const auto original_target_start =
                timebase::TickPosition{pending.target_start.value - pending.left_trim};
            const auto original_sample = tempo_map_.ticks_to_samples(original_target_start).value;
            const auto clipped_sample = tempo_map_.ticks_to_samples(pending.target_start).value;
            if (clipped_sample < original_sample)
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.child.id()}};
            std::uint64_t sample_distance = 0;
            if (original_sample >= 0 || clipped_sample < 0) {
                sample_distance = static_cast<std::uint64_t>(clipped_sample - original_sample);
            } else {
                const auto negative_magnitude =
                    static_cast<std::uint64_t>(-(original_sample + 1)) + 1;
                sample_distance = negative_magnitude + static_cast<std::uint64_t>(clipped_sample);
            }
            const auto* asset = project_.find_asset(media->asset_id);
            if (!asset)
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       media->asset_id}};
            const auto source_frames_per_timeline_frame = static_cast<double>(
                asset->sample_rate.as_long_double() / tempo_map_.sample_rate().as_long_double());
            const auto source_position = static_cast<long double>(sample_distance) *
                                         static_cast<long double>(source_frames_per_timeline_frame);
            if (source_position < 0 ||
                source_position >= static_cast<long double>(media->frame_count))
                return {};
            source_frame_offset = static_cast<double>(source_position);
        }
        auto playback = pending.child.playback_properties();
        playback.fade_in_duration =
            pending.left_trim >= static_cast<std::int64_t>(playback.fade_in_duration)
                ? 0
                : playback.fade_in_duration - static_cast<std::uint64_t>(pending.left_trim);
        playback.fade_out_duration =
            pending.right_trim >= static_cast<std::int64_t>(playback.fade_out_duration)
                ? 0
                : playback.fade_out_duration - static_cast<std::uint64_t>(pending.right_trim);
        const auto duration = static_cast<std::uint64_t>(pending.target_duration.value);
        playback.fade_in_duration = std::min(playback.fade_in_duration, duration);
        playback.fade_out_duration = std::min(playback.fade_out_duration, duration);
        auto flattened =
            timeline::Clip::create(pending.generated_id, pending.target_start,
                                   pending.target_duration, std::move(content), playback,
                                   pending.child.time_conform());
        if (!flattened)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                   pending.child.id()}};
        return append(std::move(flattened).value(), pending.child.id(), source_frame_offset,
                      pending.context_sequence_id, pending.clipped_start);
    }

    const timeline::Project& project_;
    const timebase::CompiledTempoMap& tempo_map_;
    std::uint64_t max_expanded_note_events_ = 0;
    std::uint64_t max_expanded_clips_ = 0;
    std::uint64_t expanded_note_events_ = 0;
    std::uint64_t inspected_note_events_ = 0;
    std::uint64_t expanded_clips_ = 0;
    std::uint64_t next_generated_id_ = 0;
    const timeline::Track* root_ = nullptr;
    std::vector<LoweredClip>* output_ = nullptr;
    std::size_t root_clip_index_ = 0;
    std::vector<ReferenceFrame> frames_;
    std::optional<PendingLeaf> pending_leaf_;
};

SequenceContentLowerer::SequenceContentLowerer(const timeline::Project& project,
                                               const timebase::CompiledTempoMap& tempo_map,
                                               std::uint64_t max_expanded_note_events,
                                               std::uint64_t max_expanded_clips)
    : impl_(std::make_unique<Impl>(project, tempo_map, max_expanded_note_events,
                                   max_expanded_clips)) {}

SequenceContentLowerer::~SequenceContentLowerer() = default;
SequenceContentLowerer::SequenceContentLowerer(SequenceContentLowerer&&) noexcept = default;
SequenceContentLowerer&
SequenceContentLowerer::operator=(SequenceContentLowerer&&) noexcept = default;

SequenceContentLowerer::BeginTrackResult
SequenceContentLowerer::begin_track(const timeline::Track& track,
                                    std::vector<LoweredClip>& output) {
    return impl_->begin_track(track, output);
}

std::optional<SequenceLoweringError>
SequenceContentLowerer::charge_reused(std::uint64_t clips, std::uint64_t note_events,
                                      std::uint64_t generated_ids, timeline::ItemId track_id) {
    return impl_->charge_reused(clips, note_events, generated_ids, track_id);
}

std::uint64_t SequenceContentLowerer::expanded_clip_count() const noexcept {
    return impl_->expanded_clip_count();
}

std::uint64_t SequenceContentLowerer::expanded_note_event_count() const noexcept {
    return impl_->expanded_note_event_count();
}

std::uint64_t SequenceContentLowerer::next_generated_id() const noexcept {
    return impl_->next_generated_id();
}

SequenceContentLowerer::StepResult SequenceContentLowerer::step() {
    return impl_->step();
}

} // namespace pulp::playback
