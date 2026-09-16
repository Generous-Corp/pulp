#include "sequence_content_lowerer.hpp"

#include <pulp/timebase/quantize.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::playback {

namespace {

/// Whether a flattened leaf of this content kind actually consumes clip gain.
///
/// A `MediaRef` leaf's gain reaches the renderer as
/// `AudioClipRendererProgram::gain_linear`, so a composed factor survives
/// flattening and is audible. An `EmptyContent` leaf sounds nothing, so scaling
/// it neither loses nor changes anything. Every other kind compiles to events —
/// notes, registered-content fragments, an opaque payload — and no renderer
/// scales an event by the gain of the clip that carried it, so folding a child
/// fader into one would discard it with nothing to read the loss from.
bool consumes_clip_gain(const timeline::ClipContent& content) noexcept {
    return std::holds_alternative<timeline::MediaRef>(content) ||
           std::holds_alternative<timeline::EmptyContent>(content);
}

/// Appends `placement`'s own fade ramps, translated into owner-timeline ticks.
///
/// `translation` carries a tick of the coordinate space the placement was
/// authored in to the flattened timeline; it is zero at the compile root and
/// the leaf's re-placement offset at every level below, which is a pure
/// translation because flattening moves a window without scaling it.
///
/// The ramps are read off the placement's untrimmed extent on purpose. A
/// trimmed placement enters its fade part way up, and that is exactly the fact
/// the window carries — clamping the ramp to the surviving window instead would
/// re-anchor it to a new edge and sound the fade the author did not write.
void append_placement_fades(std::vector<LoweredPlacementFade>& spans,
                            const timeline::Clip& placement, std::int64_t translation) {
    const auto playback = placement.playback_properties();
    const auto duration = static_cast<std::uint64_t>(placement.duration().value);
    const auto shape = playback.fade_shape;
    const auto shift = timebase::TickDuration{translation};
    const auto start = placement.start() + shift;
    const auto end = placement.end() + shift;
    if (const auto head = std::min(playback.fade_in_duration, duration); head != 0)
        spans.push_back(
            {start, start + timebase::TickDuration{static_cast<std::int64_t>(head)}, shape});
    if (const auto tail = std::min(playback.fade_out_duration, duration); tail != 0)
        spans.push_back(
            {end, end - timebase::TickDuration{static_cast<std::int64_t>(tail)}, shape});
}

/// Whether `span` changes what a clip occupying `[start, end)` sounds.
///
/// A ramp the clip sits entirely past reads unity everywhere the clip plays, so
/// carrying it would cost a division per sample to multiply by one. A clip on
/// the silent side is a different matter and is kept: there the ramp is what
/// makes it quiet.
bool span_reaches(const LoweredPlacementFade& span, timebase::TickPosition start,
                  timebase::TickPosition end) noexcept {
    return span.open_tick > span.silent_tick ? start < span.open_tick : end > span.open_tick;
}

/// The greatest number of ticks `GrooveTemplate::apply_timing` can move any
/// position under `groove`.
///
/// Swing warps a position toward its pair's pivot, so the extreme displacement
/// is the pivot's own distance from the grid line, and it is attained exactly at
/// the pair midpoint. That is why one evaluation there reads the supremum rather
/// than a sample of it. A per-step table adds at most its widest authored
/// offset, and the two compose additively.
///
/// Timing strength scales both terms and never magnifies, so the unscaled sum
/// still bounds the scaled result; strength is consulted only for the degenerate
/// zero, where the groove provably moves nothing at all.
///
/// The result is a supremum, not an estimate: a selection window widened by this
/// many ticks cannot miss a note the groove would have pulled into view.
std::int64_t groove_timing_reach(const timeline::GrooveTemplate& groove) noexcept {
    if (groove.states_no_feel() || groove.timing_strength() == 0)
        return 0;
    std::int64_t reach = 0;
    if (const auto grid = groove.swing_grid(); grid.value != 0) {
        const auto extreme = timebase::swing_displacement(timebase::TickPosition{grid.value}, grid,
                                                          groove.swing());
        reach = extreme.value < 0 ? -extreme.value : extreme.value;
    }
    std::int64_t widest_step = 0;
    for (const auto& step : groove.steps()) {
        const auto offset = step.timing_offset.value;
        widest_step = std::max(widest_step, offset < 0 ? -offset : offset);
    }
    return reach + widest_step;
}

/// Every way a nesting can change what a sealed artifact sounds.
///
/// A sealed artifact — a track freeze, or the takes a comp selects — is
/// rendered and anchored in absolute samples, so nothing about it can be
/// re-derived: it either lands where it was rendered to land, or it is a stale
/// render playing at the wrong time or level. That makes the
/// question "does this nesting transform its child at all?" rather than "can we
/// map the artifact through the transform?", and this enum is the answer's
/// vocabulary — one enumerator per observation, so the predicate below is a
/// list nobody can shorten by accident.
///
/// Membership is deliberately wider than the transformations the walk applies
/// today. `PlacementConform` and the three modulation entries are read by
/// neither this walk nor `begin_track`, so a child carrying one is neither
/// honoured nor refused anywhere else; without an entry here a modulated fader
/// under a sealed artifact would be silently permitted.
enum class NestingTransformation : std::uint8_t {
    PlacementTranslation,
    PlacementWindowStart,
    PlacementWindowEnd,
    PlacementGain,
    PlacementFade,
    PlacementConform,
    PlacementAnchor,
    TrackFader,
    TrackPan,
    TrackDeviceChain,
    TrackAutomationLane,
    TrackModulator,
    TrackMacro,
    TrackModulationRoute,
    TrackTuning,
    SequenceGroove,
    SequenceDynamics,
    SequenceChordScale,
    ArtifactRate,
    kCount,
};

/// The sealed artifact a nesting is being asked to carry.
///
/// A freeze and a selected take lane are one construct with two payloads: a
/// track-scoped rendered artifact anchored in absolute samples, chosen as the
/// alternative to flattening. The eighteen nesting observations above are
/// therefore the same eighteen questions for both, and are asked once.
///
/// Only `ArtifactRate` has to know which payload it holds, because a freeze
/// declares one rate and a comp declares one per take its segments draw from.
/// Exactly one member is set; neither being set is refused rather than
/// permitted, in the same direction as the switch's trailing `return true`.
struct SealedArtifact {
    const timeline::TrackFreeze* freeze = nullptr;
    const timeline::TakeLane* active_take = nullptr;
};

} // namespace

/// Flattens `SequenceRef` placements into leaf clips on the referring track.
///
/// ## Gain precedence through a nested placement
///
/// Nesting stacks gain stages in series — the placement clip that admits the
/// child window, the child track's own fader, and the leaf clip's authored gain
/// — so the flattened leaf carries their **product**:
///
///     leaf.gain = placement.gain * child_track.gain * leaf.gain
///
/// Multiplication is the rule because that is what the stages mean when they
/// are not flattened: a child sequence feeding a parent is a submix bus, and a
/// -6 dB fader under a -6 dB placement plays at -12 dB. Nothing overrides
/// anything, so the result does not depend on the order the stages are read,
/// and nesting one more level deep composes by multiplying again. Unity at any
/// stage is exactly the identity, so a transparent nesting produces the same
/// float the leaf authored rather than a rounded near-miss.
///
/// Two neighbouring pieces of mixer state deliberately do **not** compose, and
/// each keeps a distinct refusal rather than being folded away:
///
///  * **Pan** (`NestedMixerPanUnsupported`) — a clip has no stereo placement of
///    its own, and the parent track's single pan also serves whatever else that
///    track holds. There is no value to write it into.
///  * **A placement fade over a leaf that consumes no clip gain**
///    (`NestedPlacementFadeUnsupported`) — the fade does compose, but only
///    where something reads it. A fade is a time-varying gain, so it needs the
///    sink static gain needs, and note, registered and opaque leaves compile to
///    events no renderer scales by the gain of the clip that carried them.
///
/// The fade over a leaf that *does* consume clip gain composes, and it does so
/// without folding: a placement fade travels beside the leaf as the ramp it
/// is, in owner-timeline ticks, and the renderer reads the leaf's position
/// within it. Gain can fold because a product of scalars is a scalar; a ramp is
/// time-varying, and two ramps of different shapes do not reduce to a third, so
/// the leaf carries the list. Nesting one level deeper appends to it.
///
/// A leaf's *own* fade is a different thing again and composes multiplicatively
/// with the above: a trim that cuts into it re-anchors the fade to the new edge
/// and shortens it by the trim, the same answer an unnested clip gives when it
/// is dragged shorter. The placement ramp keeps its own edges instead, which is
/// why a trimmed placement enters its fade part way up.
///
/// And a composed gain that has nowhere to land is refused rather than dropped
/// (`NestedGainSinkUnsupported`): see `consumes_clip_gain`.
///
/// A child track's *automated* gain does not compose at all, because a curve
/// has no scalar to fold into. It travels to the leaf as the bare fact that one
/// exists, and is refused there by the leaf's own kind: a leaf that reads no
/// clip gain raises `NestedAutomationGainEventLeafUnsupported`, and one that
/// does raises `NestedAutomationGainMediaUnsupported` because the field it
/// reads is a single scalar. An automated pan needs no leaf to decide it and is
/// refused on entry to the child track (`NestedAutomationPanUnsupported`).
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
        push_reference(clip, *reference, 1, 1.0f, false, {}, clip, 0);
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
        // Product of every enclosing placement's clip gain and every enclosing
        // child track's fader, down to but excluding the track being walked.
        float inherited_gain = 1.0f;
        // The fader of the child track currently being walked, set once when
        // the walk enters it.
        float track_gain = 1.0f;
        // Whether any enclosing child track automates its gain, down to but
        // excluding the track being walked. A curve cannot fold into the
        // scalar the gain product folds into, so this travels as the bare fact
        // that one exists and is answered at the leaf, where the leaf's own
        // kind decides which refusal applies.
        bool inherited_gain_automation = false;
        // Whether the child track currently being walked automates its gain,
        // set once when the walk enters it.
        bool track_gain_automation = false;
        // Every enclosing placement's fade ramps, this frame's own included,
        // already carried into the owner timeline's ticks. Each level appends;
        // nothing folds, because ramps of different shapes have no common
        // shape to fold into.
        std::vector<LoweredPlacementFade> placement_fades;
        // The child track this placement was authored on, or null when the
        // placement sits on the compile root. Only the intermediate tracks are
        // recorded, because the root track's own state applies to a nested and
        // an unnested document alike and so is never a difference the nesting
        // introduced.
        const timeline::Track* owning_track = nullptr;
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
        // Ticks of content kept beyond each cut edge so the owner's groove has
        // something to pull inward. Never wider than the trim it sits behind.
        std::int64_t pad_left = 0;
        std::int64_t pad_right = 0;
        std::size_t note_index = 0;
        std::vector<timeline::NoteEvent> clipped_notes;
        timeline::ItemId context_sequence_id;
        // Everything the nesting contributes on top of this leaf's own gain.
        // Exactly 1.0f when the nesting is transparent, which is the value that
        // makes composition a no-op rather than a rounding event.
        float composed_gain = 1.0f;
        // The enclosing ramps that reach this leaf, in owner-timeline ticks.
        std::vector<LoweredPlacementFade> placement_fades;
    };

    StepResult append(timeline::Clip clip, timeline::ItemId source,
                      double source_frame_offset = 0.0, timeline::ItemId context_sequence_id = {},
                      std::optional<timebase::TickPosition> context_start = std::nullopt,
                      std::int64_t groove_pad_left = 0, std::int64_t groove_pad_right = 0,
                      std::vector<LoweredPlacementFade> placement_fades = {},
                      std::int64_t authored_window_start = 0,
                      timebase::TickDuration authored_duration = {},
                      double source_frame_phase_end = 0.0) {
        if (expanded_clips_ >= max_expanded_clips_)
            return {.error =
                        SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded, source}};
        ++expanded_clips_;
        const auto authored_start = context_start.value_or(clip.start());
        // A clip no nesting trimmed is its own authored extent, so the window
        // is the identity rather than a case the compiler has to special-case.
        const auto authored = authored_duration.value > 0 ? authored_duration : clip.duration();
        output_->push_back({std::move(clip), source_frame_offset, context_sequence_id,
                            authored_start, groove_pad_left, groove_pad_right,
                            std::move(placement_fades), authored_window_start, authored,
                            source_frame_phase_end});
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
        // Neither guard below is reachable through a validly-constructed
        // Project, so each names InvalidStructure rather than a capability
        // code: the document should not exist, which is a stronger statement
        // than "playback cannot express this yet". They are defensive
        // backstops kept because this walk builds placements of its own.
        //
        // validate_sequence_graph rejects a graph deeper than
        // kMaxSequenceNestingDepth before a Project can hold it.
        if (depth > timeline::kMaxSequenceNestingDepth)
            return SequenceLoweringError{CompileErrorCode::InvalidStructure, placement.id()};
        // Clip::create_absolute refuses SequenceRef content outright, so a
        // clip carrying a reference is always musically anchored.
        if (placement.time_anchor() != timeline::ClipTimeAnchor::Musical)
            return SequenceLoweringError{CompileErrorCode::InvalidStructure, placement.id()};
        const auto playback = placement.playback_properties();
        // A placement fade travels beside the leaves as a ramp rather than
        // entering them, so nothing about the envelope itself is refused here;
        // the one case with nowhere to land is caught per leaf, where the
        // content that would have to read it is known.
        //
        // Gain composes into the flattened leaves instead; the class comment
        // states the precedence rule. Only the values that cannot enter a
        // product at all are rejected here.
        if (!std::isfinite(playback.gain_linear) || playback.gain_linear < 0.0f)
            return SequenceLoweringError{CompileErrorCode::InvalidStructure, placement.id()};
        if (!project_.find_sequence(reference.sequence_id))
            return SequenceLoweringError{CompileErrorCode::InvalidStructure, reference.sequence_id};
        if (reference.source_start.value >
            std::numeric_limits<std::int64_t>::max() - placement.duration().value)
            return SequenceLoweringError{CompileErrorCode::InvalidStructure, placement.id()};
        return std::nullopt;
    }

    /// `authored_placement` is the placement as written, before any trim this
    /// walk applied to it, and `translation` carries its ticks into the owner
    /// timeline. The two are separate from `placement` because the clip pushed
    /// onto the stack is the trimmed, re-placed one whose fades have already
    /// been lifted out of it and into the ramp list.
    void push_reference(const timeline::Clip& placement, const timeline::SequenceRef& reference,
                        std::size_t depth, float inherited_gain, bool inherited_gain_automation,
                        std::vector<LoweredPlacementFade> placement_fades,
                        const timeline::Clip& authored_placement, std::int64_t translation,
                        const timeline::Track* owning_track = nullptr) {
        append_placement_fades(placement_fades, authored_placement, translation);
        auto& frame = frames_.emplace_back(ReferenceFrame{
            placement, reference, project_.find_sequence(reference.sequence_id),
            reference.source_start + timebase::TickDuration{placement.duration().value}, depth});
        frame.inherited_gain = inherited_gain * placement.playback_properties().gain_linear;
        frame.inherited_gain_automation = inherited_gain_automation;
        frame.placement_fades = std::move(placement_fades);
        frame.owning_track = owning_track;
    }

    /// Whether the nesting currently on the frame stack imposes `which`.
    ///
    /// Every enumerator answers for the whole stack, not just the innermost
    /// frame: a transparent placement inside a trimmed one is still a trimmed
    /// nesting, and the artifact underneath cannot tell the two apart.
    ///
    /// An enumerator with no case below reaches the trailing `return true` and
    /// is therefore reported as imposed. That direction is the safe one — a
    /// transformation nobody has reasoned about refuses the nesting instead of
    /// permitting it — and it is what makes adding an enumerator fail closed by
    /// construction rather than by anyone remembering to extend this function.
    bool nesting_imposes(NestingTransformation which, const timeline::Track& track,
                         const SealedArtifact& artifact) const {
        const auto any_frame = [this](auto&& predicate) {
            return std::any_of(frames_.begin(), frames_.end(), predicate);
        };
        // The track being walked plus every intermediate track the walk passed
        // through to reach it. A fader or a modulator two levels up transforms
        // the artifact exactly as much as one directly above it, and only the
        // compile root's own track state is excluded — that applies to a nested
        // and an unnested document alike, so it is never a difference the
        // nesting introduced.
        const auto any_track = [this, &track](auto&& predicate) {
            return predicate(track) ||
                   std::any_of(frames_.begin(), frames_.end(), [&](const ReferenceFrame& frame) {
                       return frame.owning_track != nullptr && predicate(*frame.owning_track);
                   });
        };
        switch (which) {
        case NestingTransformation::PlacementTranslation:
            // Child tick T reaches owner tick placement.start() + (T -
            // source_start). Anything but the identity moves musical content
            // that the artifact, having no tick position at all, cannot follow.
            return any_frame([](const ReferenceFrame& frame) {
                return frame.placement.start().value != frame.reference.source_start.value;
            });
        case NestingTransformation::PlacementWindowStart:
            return any_frame([](const ReferenceFrame& frame) {
                return frame.reference.source_start.value != 0;
            });
        case NestingTransformation::PlacementWindowEnd:
            // A window that ends before the child does cuts content. The cut is
            // expressed in ticks and the artifact in samples, so there is no
            // honest way to apply it; a child that declares no duration cannot
            // prove the window covers it and is refused for that reason.
            return any_frame([](const ReferenceFrame& frame) {
                const auto duration = frame.sequence->duration();
                return !duration || frame.source_end.value < duration->value;
            });
        case NestingTransformation::PlacementGain:
            // Each placement is asked separately rather than reading the
            // accumulated product, so a 2.0 above a 0.5 is refused instead of
            // cancelling to a unity nobody authored.
            return any_frame([](const ReferenceFrame& frame) {
                return frame.placement.playback_properties().gain_linear != 1.0f;
            });
        case NestingTransformation::PlacementFade:
            // append_placement_fades pushes nothing for a fadeless placement, so
            // an empty accumulated list is exactly "no enclosing placement fades".
            return !frames_.empty() && !frames_.back().placement_fades.empty();
        case NestingTransformation::PlacementConform:
            return any_frame([](const ReferenceFrame& frame) {
                return frame.placement.time_conform() != timeline::TimeConform::None;
            });
        case NestingTransformation::PlacementAnchor:
            return any_frame([](const ReferenceFrame& frame) {
                return frame.placement.time_anchor() != timeline::ClipTimeAnchor::Musical;
            });
        case NestingTransformation::TrackFader:
            return any_track([](const timeline::Track& walked) {
                return walked.mixer().gain_linear != 1.0f;
            });
        case NestingTransformation::TrackPan:
            return any_track(
                [](const timeline::Track& walked) { return walked.mixer().pan != 0.0f; });
        case NestingTransformation::TrackDeviceChain:
            return any_track(
                [](const timeline::Track& walked) { return !walked.device_chain().empty(); });
        case NestingTransformation::TrackAutomationLane:
            return any_track(
                [](const timeline::Track& walked) { return !walked.automation_lanes().empty(); });
        case NestingTransformation::TrackModulator:
            return any_track(
                [](const timeline::Track& walked) { return !walked.modulators().empty(); });
        case NestingTransformation::TrackMacro:
            return any_track(
                [](const timeline::Track& walked) { return !walked.macros().empty(); });
        case NestingTransformation::TrackModulationRoute:
            return any_track(
                [](const timeline::Track& walked) { return !walked.modulation_routes().empty(); });
        case NestingTransformation::TrackTuning:
            return any_track(
                [](const timeline::Track& walked) { return walked.tuning().has_value(); });
        case NestingTransformation::SequenceGroove:
            return any_frame([](const ReferenceFrame& frame) {
                return !frame.sequence->groove().states_no_feel();
            });
        case NestingTransformation::SequenceDynamics:
            return any_frame(
                [](const ReferenceFrame& frame) { return !frame.sequence->dynamics_lane().empty(); });
        case NestingTransformation::SequenceChordScale:
            return any_frame([](const ReferenceFrame& frame) {
                return !frame.sequence->chord_scale_lane().empty();
            });
        case NestingTransformation::ArtifactRate:
            return artifact_rate_differs(artifact);
        case NestingTransformation::kCount:
            break;
        }
        return true;
    }

    /// Whether the artifact's declared rate differs from either rate the two
    /// emission paths read.
    ///
    /// This one is not a transformation the owner applies, and it is here
    /// because leaving it out silently breaks the identity this predicate
    /// exists to guarantee. A top-level artifact compiles through
    /// compile_track_freeze_program or compile_take_comp_segment_program, both
    /// of which set the renderable length to the artifact's projected timeline
    /// span; a lowered leaf compiles through the generic absolute-clip path,
    /// which sets it to the source length scaled and rounded up. Those two
    /// agree only when no rate conversion happens at all — otherwise they can
    /// differ by a frame, and the nested render stops being the unnested one.
    /// Do not delete this as redundant with either compiler's own rate check:
    /// those run on a path this leaf never takes.
    bool artifact_rate_differs(const SealedArtifact& artifact) const {
        const auto timeline_rate = tempo_map_.sample_rate().normalized();
        // Both rates, because the leaf's generic path reads the decoded
        // asset's rate where the artifact compilers read the declared one. The
        // document ties the two together, so a disagreement is not reachable
        // through a valid project; asking anyway costs a lookup and removes an
        // assumption from a predicate whose whole value is that it assumes
        // nothing.
        const auto matches = [&](timebase::RationalRate declared, timeline::ItemId asset_id) {
            const auto normalized = declared.normalized();
            if (normalized != timeline_rate)
                return false;
            const auto* asset = project_.find_asset(asset_id);
            return asset != nullptr && asset->sample_rate.normalized() == normalized;
        };
        if (artifact.freeze != nullptr)
            return !matches(artifact.freeze->sample_rate, artifact.freeze->media.asset_id);
        if (artifact.active_take != nullptr) {
            // Every take a segment draws from, and only those. A lane may hold
            // takes the comp never selects, and a rate those carry is not a
            // rate this nesting would have to convert.
            //
            // A comp selecting nothing converts nothing, so an empty one is
            // not a differing rate. It lowers to no leaves, which is what the
            // unnested lane renders too.
            for (const auto& segment : artifact.active_take->comp_segments()) {
                const auto* take = artifact.active_take->find_take(segment.take_id);
                if (take == nullptr || !matches(take->sample_rate(), take->media().asset_id))
                    return true;
            }
            return false;
        }
        // Neither payload set. Same direction as the switch's trailing
        // `return true`: an artifact kind nobody has reasoned about refuses the
        // nesting rather than permitting it.
        return true;
    }

    /// Whether this nesting leaves its child exactly as authored.
    ///
    /// The loop is the point: permission is granted only by every enumerator
    /// answering "not imposed", so an unanswered one cannot reach `true`.
    bool nesting_is_transparent(const timeline::Track& track,
                                const SealedArtifact& artifact) const {
        for (auto index = static_cast<std::uint8_t>(0);
             index < static_cast<std::uint8_t>(NestingTransformation::kCount); ++index)
            if (nesting_imposes(static_cast<NestingTransformation>(index), track, artifact))
                return false;
        return true;
    }

    /// Emits a transparently nested freeze as the sealed artifact it already is.
    ///
    /// The artifact is absolute, so it lowers to an absolute leaf carrying the
    /// same media over the same samples rather than to anything derived from
    /// the child's arrangement. Skipping to the next track is what makes this a
    /// substitution rather than an addition — the nested analogue of
    /// begin_track discarding the clips it replaced.
    StepResult emit_sealed_freeze(ReferenceFrame& frame, const timeline::Track& track) {
        const auto& freeze = *track.freeze();
        if (next_generated_id_ == 0 ||
            next_generated_id_ == std::numeric_limits<std::uint64_t>::max())
            return {.error =
                        SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded, track.id()}};
        auto leaf = timeline::Clip::create_absolute(timeline::ItemId{next_generated_id_++},
                                                    freeze.placement_start, freeze.media.frame_count,
                                                    freeze.sample_rate, freeze.media);
        if (!leaf)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure, track.id()}};
        auto appended = append(std::move(leaf).value(), track.id());
        if (appended.error)
            return appended;
        ++frame.track_index;
        frame.clip_index = 0;
        return {};
    }

    /// Emits a transparently nested active take comp as the sealed windows it
    /// already is.
    ///
    /// The freeze above lowers to one leaf; a comp lowers to N, because it is a
    /// sequence of absolute windows each drawn from its own take. Per segment:
    /// resolve the take the segment names, read the source offset as the
    /// distance from that take's placement to the segment's start, and emit an
    /// absolute leaf carrying exactly that window of the take's media.
    ///
    /// This is the same arithmetic compile_take_comp_segment_program performs,
    /// re-derived rather than shared, because the two stand on opposite sides
    /// of the compiler: that one builds a renderer program, this one builds a
    /// document clip the renderer has yet to compile. What they owe each other
    /// is the rendered samples, so a test asserts that identity instead of a
    /// comment asserting the arithmetic.
    ///
    /// Every bound below was already proved by TakeLane::canonical_comp before
    /// the lane could be constructed, so each names InvalidStructure — the
    /// document should not exist — rather than a capability refusal, and is a
    /// backstop against this walk building a leaf the model never sanctioned.
    StepResult emit_sealed_active_take(ReferenceFrame& frame, const timeline::Track& track,
                                       const timeline::TakeLane& lane) {
        for (const auto& segment : lane.comp_segments()) {
            const auto* take = lane.find_take(segment.take_id);
            if (take == nullptr || segment.range.start.value < take->placement_start().value)
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, lane.id()}};
            const auto& media = take->media();
            const auto offset = static_cast<std::uint64_t>(segment.range.start.value -
                                                           take->placement_start().value);
            if (offset > media.frame_count ||
                segment.range.sample_count > media.frame_count - offset)
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, take->id()}};
            if (media.source_start.value < 0 ||
                offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() -
                                                    media.source_start.value))
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, take->id()}};
            if (next_generated_id_ == 0 ||
                next_generated_id_ == std::numeric_limits<std::uint64_t>::max())
                return {.error = SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded,
                                                       track.id()}};
            // The take's rate, not the segment's. They are equal — canonical_comp
            // rejects a comp whose segment rate is not its take's — so this is a
            // choice of which authority to read, and the segment compiler reads
            // the take's when it projects the window onto the timeline.
            auto leaf = timeline::Clip::create_absolute(
                timeline::ItemId{next_generated_id_++}, segment.range.start,
                segment.range.sample_count, take->sample_rate(),
                timeline::MediaRef{media.asset_id,
                                   timebase::SamplePosition{media.source_start.value +
                                                            static_cast<std::int64_t>(offset)},
                                   segment.range.sample_count});
            if (!leaf)
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, take->id()}};
            if (auto appended = append(std::move(leaf).value(), track.id()); appended.error)
                return appended;
        }
        ++frame.track_index;
        frame.clip_index = 0;
        return {};
    }

    StepResult step_reference() {
        auto& frame = frames_.back();
        if (frame.track_index == frame.sequence->tracks().size()) {
            frames_.pop_back();
            return {};
        }
        const auto& track = frame.sequence->tracks()[frame.track_index];
        if (frame.clip_index == 0) {
            // Two unrelated constructs that once shared one code. A device
            // chain needs a sub-bus to survive flattening; an automation lane
            // needs a per-clip curve sink. Each names its own code so the
            // refusal says which construct is missing.
            if (!track.device_chain().empty())
                return {.error = SequenceLoweringError{
                            CompileErrorCode::NestedDeviceChainUnsupported, track.id()}};
            // The chain is empty by the guard above, and Track::create rejects
            // a lane whose device placement is absent from that chain, so every
            // lane reaching this walk targets the track's own mixer. Pan is
            // refused outright: no leaf carries a stereo placement at any
            // level, so no leaf kind can change the answer. Gain travels to the
            // leaf instead, because there the leaf's own kind decides whether
            // the sink is merely a scalar or absent altogether, and those are
            // separately closable.
            bool gain_automation = false;
            for (const auto& lane : track.automation_lanes()) {
                const auto* mixer_target = std::get_if<timeline::TrackMixerTarget>(&lane.target());
                if (!mixer_target)
                    return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                           lane.id()}};
                switch (mixer_target->parameter) {
                case timeline::TrackMixerParameter::Pan:
                    return {.error = SequenceLoweringError{
                                CompileErrorCode::NestedAutomationPanUnsupported, track.id()}};
                case timeline::TrackMixerParameter::Gain:
                    gain_automation = true;
                    break;
                }
            }
            // Freeze and an active take lane are the two states that replace a
            // track's arrangement with something else. begin_track honours that
            // replacement by returning Freeze or ActiveTake content and
            // discarding the clips; this walk descends into the clips instead,
            // so it would play precisely the material the author replaced.
            //
            // They keep separate codes because the reason a reader hits each is
            // different — a freeze is one media placement, an active take is N
            // comp segments each needing take resolution — not because the
            // construct that lifts them differs. It does not: both are a
            // track-scoped sealed artifact anchored in absolute samples,
            // selected as the alternative to flattening. One construct, two
            // payloads.
            //
            // A sealed artifact carries no tick position, so it cannot follow a
            // nesting that moves or cuts its child. Where the nesting provably
            // moves and cuts nothing, the artifact is already in the right
            // place and lowers as itself; everywhere else the refusal stands.
            // nesting_is_transparent is the whole of that judgement, and it
            // grants permission only by affirmative match.
            if (track.freeze()) {
                if (!nesting_is_transparent(track, SealedArtifact{.freeze = &*track.freeze()}))
                    return {.error = SequenceLoweringError{
                                CompileErrorCode::NestedFrozenTrackUnsupported, track.id()}};
                return emit_sealed_freeze(frame, track);
            }
            if (track.active_take_lane_id().valid()) {
                const auto* lane = track.find_take_lane(track.active_take_lane_id());
                // A selection naming no lane is a broken document rather than a
                // nesting this walk declines to carry, and begin_track says the
                // same about the same document at the top level.
                if (lane == nullptr)
                    return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                           track.active_take_lane_id()}};
                if (!nesting_is_transparent(track, SealedArtifact{.active_take = lane}))
                    return {.error = SequenceLoweringError{
                                CompileErrorCode::NestedActiveTakeUnsupported, track.id()}};
                return emit_sealed_active_take(frame, track, *lane);
            }
            // Record-arm and unselected take lanes are deliberately absent from
            // the refusals above. Neither reaches lowered output at either
            // level: begin_track consults freeze and the active lane and never
            // reads record-arm or the lane list, so a nested track carrying
            // them lowers to the same clips as one without. Refusing them
            // rejected documents that already compile correctly.
            const auto mixer = track.mixer();
            // Pan has no sink. Flattening folds the child into the parent
            // track, and the parent's single pan also serves whatever else that
            // track holds, so the child's balance can be neither carried nor
            // dropped without changing something the author did not touch.
            if (mixer.pan != 0.0f)
                return {.error = SequenceLoweringError{CompileErrorCode::NestedMixerPanUnsupported,
                                                       track.id()}};
            if (!std::isfinite(mixer.gain_linear) || mixer.gain_linear < 0.0f)
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, track.id()}};
            // The fader enters the product instead of being refused; it leaves
            // the product again when the walk moves to the next track, because
            // this assignment runs once per track entry. The gain curve travels
            // beside it as a bare fact rather than a value, for want of a scalar
            // it could fold into.
            frame.track_gain = mixer.gain_linear;
            frame.track_gain_automation = gain_automation;
        }
        if (frame.clip_index == track.clips().size()) {
            ++frame.track_index;
            frame.clip_index = 0;
            return {};
        }

        const auto& child = track.clips()[frame.clip_index++];
        if (auto charged = charge_reference(child.id()); charged.error)
            return charged;
        // Unlike the placement guard in validate_reference, this one is
        // reachable: a leaf clip inside a nested sequence can genuinely be
        // absolute-anchored, and flattening it onto a musical owner has no
        // hybrid time domain to write.
        if (child.time_anchor() != timeline::ClipTimeAnchor::Musical)
            return {.error = SequenceLoweringError{
                        CompileErrorCode::NestedAbsoluteChildUnsupported, child.id()}};
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
        // Everything the nesting adds on top of this leaf's own authored gain.
        const auto composed_gain = frame.inherited_gain * frame.track_gain;
        // Whether any level of the nesting reaching this leaf automates gain.
        // Scalars compose by multiplying; curves cannot, so this composes by
        // disjunction and is answered once, here, where the leaf kind is known.
        const auto composed_gain_automation =
            frame.inherited_gain_automation || frame.track_gain_automation;
        // A conforming clip maps its complete authored source span onto its
        // musical placement, so a retained window maps onto the matching
        // sub-span of the source under the same function. Resample names that
        // sub-span below, as a source range travelling beside the clip.
        //
        // Stretch reaches the same window from the other end. Its audio is a
        // rendered artifact keyed to the clip's own authored tick range, so the
        // renderer keeps rendering that range — the authored window already
        // travelling beside the leaf says where it is — and the leaf reads the
        // matching frames of the result. Re-keying the artifact to the trimmed
        // range would re-stretch the whole source into it and play the wrong
        // audio at every frame.

        if (const auto* nested = std::get_if<timeline::SequenceRef>(&child.content())) {
            if (nested->source_start.value > std::numeric_limits<std::int64_t>::max() - left_trim)
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, child.id()}};
            // The inner placement's own fades are lifted out of the clip and
            // into the ramp list, which is the only place they can survive a
            // trim intact: the clip carries the trimmed window, and a fade
            // wider than that window is not a clip a document can hold, so
            // leaving them here would reject a nesting that renders perfectly
            // well. Its gain stays, because gain does fold.
            auto nested_playback = child.playback_properties();
            nested_playback.fade_in_duration = 0;
            nested_playback.fade_out_duration = 0;
            auto nested_clip = timeline::Clip::create(
                child.id(), target_start, target_duration,
                timeline::SequenceRef{nested->sequence_id,
                                      nested->source_start + timebase::TickDuration{left_trim}},
                nested_playback);
            if (!nested_clip)
                return {.error =
                            SequenceLoweringError{CompileErrorCode::InvalidStructure, child.id()}};
            const auto reference = std::get<timeline::SequenceRef>(nested_clip->content());
            const auto depth = frame.depth + 1;
            if (const auto error = validate_reference(nested_clip.value(), reference, depth))
                return {.error = error};
            if (auto charged = charge_reference(child.id()); charged.error)
                return charged;
            push_reference(nested_clip.value(), reference, depth, composed_gain,
                           composed_gain_automation, frame.placement_fades, child,
                           target_start.value - clipped_start.value, &track);
            return {};
        }

        if (composed_gain != 1.0f && !consumes_clip_gain(child.content()))
            return {.error = SequenceLoweringError{CompileErrorCode::NestedGainSinkUnsupported,
                                                   child.id()}};
        // A gain curve somewhere above this leaf. The two ways it fails are
        // different constructs and close separately, so each names its own
        // code: a leaf that reads no clip gain needs a renderer that scales it
        // before any envelope would matter, while a leaf that does read clip
        // gain needs only that the field it reads stop being a single scalar.
        if (composed_gain_automation)
            return {.error = SequenceLoweringError{
                        consumes_clip_gain(child.content())
                            ? CompileErrorCode::NestedAutomationGainMediaUnsupported
                            : CompileErrorCode::NestedAutomationGainEventLeafUnsupported,
                        child.id()}};
        // Only the ramps this leaf lies within travel with it. One the leaf
        // sits wholly past reads unity for its whole extent, and a leaf that
        // never meets a ramp is a leaf that need not know one exists.
        std::vector<LoweredPlacementFade> leaf_fades;
        for (const auto& span : frame.placement_fades)
            if (span_reaches(span, target_start, target_start + target_duration))
                leaf_fades.push_back(span);
        // A fade is a time-varying gain, so it needs the sink a static gain
        // needs, and the content kinds that have none are the same ones.
        // Refusing is the whole point: the alternative is a document that
        // compiles, plays the note leaf at full level through a fade the
        // author wrote, and reports nothing.
        if (!leaf_fades.empty() && !consumes_clip_gain(child.content()))
            return {.error = SequenceLoweringError{CompileErrorCode::NestedPlacementFadeUnsupported,
                                                   child.id()}};
        if (next_generated_id_ == 0 ||
            next_generated_id_ == std::numeric_limits<std::uint64_t>::max())
            return {.error = SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded,
                                                   child.id()}};
        // A trimmed note leaf keeps a little content beyond each cut edge, so a
        // groove that pulls a note inward across that edge still has the note to
        // pull. Padding past the child's own extent would select nothing — the
        // child holds no note outside itself — so the reach is capped by the
        // trim, which also keeps the widened window inside the child and
        // therefore free of overflow. An untrimmed edge pads by zero for the
        // same reason, and a groove that states no feel reaches zero, so both
        // cases lower to exactly the clip they lowered to before.
        std::int64_t pad_left = 0;
        std::int64_t pad_right = 0;
        if (std::holds_alternative<timeline::MidiContent>(child.content())) {
            const auto reach = groove_timing_reach(frame.sequence->groove());
            pad_left = std::min(reach, left_trim);
            pad_right = std::min(reach, right_trim);
        }
        pending_leaf_ = PendingLeaf{
            child,
            timeline::ItemId{next_generated_id_++},
            clipped_start,
            clipped_end,
            target_start,
            target_duration,
            left_trim,
            right_trim,
            pad_left,
            pad_right,
            0,
            {},
            frame.sequence->id(),
            composed_gain,
            std::move(leaf_fades),
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
        if (note_start > std::numeric_limits<std::int64_t>::max() - note.duration.value)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure, note.id}};
        const auto note_end = note_start + note.duration.value;
        // Selection runs over the padded window, but the note still rebases onto
        // the padded anchor, so a retained note keeps its true distance from the
        // cut edge. Whether it actually sounds is decided later, after the groove
        // has moved it and the compiler has clamped it to the audible window.
        const auto window_start = pending.clipped_start.value - pending.pad_left;
        const auto window_end = pending.clipped_end.value + pending.pad_right;
        const auto audible_start = std::max(note_start, window_start);
        const auto audible_end = std::min(note_end, window_end);
        if (audible_end <= audible_start)
            return {};
        if (expanded_note_events_ > max_expanded_note_events_ ||
            max_expanded_note_events_ - expanded_note_events_ < 2)
            return {.error = SequenceLoweringError{CompileErrorCode::ExpansionBudgetExceeded,
                                                   pending.child.id()}};
        expanded_note_events_ += 2;
        auto clipped_note = note;
        clipped_note.start = timebase::TickPosition{audible_start - window_start};
        clipped_note.duration = timebase::TickDuration{audible_end - audible_start};
        pending.clipped_notes.push_back(clipped_note);
        return {};
    }

    StepResult finish_pending_leaf() {
        auto pending = std::move(*pending_leaf_);
        pending_leaf_.reset();
        timeline::ClipContent content = pending.child.content();
        double source_frame_offset = 0.0;
        double source_frame_phase_end = 0.0;
        if (const auto* notes = std::get_if<timeline::MidiContent>(&content)) {
            const auto* owner = project_.find_sequence(pending.context_sequence_id);
            if (!owner)
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.context_sequence_id}};
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
                return std::any_of(
                    pending.clipped_notes.begin(), pending.clipped_notes.end(),
                    [&](const timeline::NoteEvent& note) { return note.id == note_id; });
            };
            std::vector<timeline::NoteModifier> modifiers;
            for (const auto& modifier : notes->modifiers())
                if (retained(modifier.note_id))
                    modifiers.push_back(modifier);
            const auto seed = notes->modifier_seed();
            // A point before the retained window can still be the value
            // sounding inside it, so neither dropping nor carrying it verbatim
            // is right. The rule is the one the lane's own storage documents:
            // the sounding value at a position is the last point at or before
            // it. So each lane keeps the points inside the window, rebased onto
            // it, and gains a derived point at its start carrying whatever was
            // last said before it. The derived point keeps the identity of the
            // entry it read, which is what makes the result reproducible rather
            // than merely plausible.
            //
            // A lane that authored nothing at or before the window start has no
            // value sounding on entry, and correctly contributes no derived
            // point — absent, not zero, because zero is itself a controller
            // value a user can author and mean.
            std::vector<timeline::MidiExpressionLane> lanes;
            for (const auto& lane : notes->lanes()) {
                timeline::MidiExpressionLane rebased{lane.id, lane.address, {}};
                const timeline::MidiLanePoint* sounding = nullptr;
                // The retained window is half-open in child-local ticks:
                // points before it establish what is sounding on entry, points
                // after it are never reached and must not be emitted, or a
                // trimmed clip would play controller values the placement
                // deliberately excluded.
                const auto window_end = pending.left_trim + pending.target_duration.value;
                for (const auto& point : lane.points) {
                    if (point.position.value < pending.left_trim) {
                        // Ordered by position, so the last one seen below the
                        // boundary is the one still sounding at it.
                        sounding = &point;
                        continue;
                    }
                    if (point.position.value >= window_end)
                        break; // ordered, so nothing later is reachable either
                    rebased.points.push_back({point.id,
                                              timebase::TickPosition{point.position.value -
                                                                     pending.left_trim},
                                              point.value, point.chased});
                }
                // A lane that authors a point exactly at the window start
                // already says what sounds on entry, so nothing is derived: the
                // authored value wins, and adding a derived point beside it
                // would put two entries at one position with no rule ordering
                // them.
                const bool authored_at_start =
                    !rebased.points.empty() && rebased.points.front().position.value == 0;
                if (sounding != nullptr && !authored_at_start)
                    rebased.points.insert(rebased.points.begin(),
                                          {sounding->id, timebase::TickPosition{0},
                                           sounding->value, true});
                if (!rebased.points.empty())
                    lanes.push_back(std::move(rebased));
            }
            auto rebuilt = timeline::MidiContent::create(std::move(pending.clipped_notes),
                                                         std::move(modifiers), seed,
                                                         std::move(lanes));
            if (!rebuilt)
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.child.id()}};
            content = std::move(rebuilt).value();
        } else if (auto* media = std::get_if<timeline::MediaRef>(&content);
                   media && pending.child.time_conform() == timeline::TimeConform::Resample &&
                   (pending.left_trim > 0 || pending.right_trim > 0)) {
            // Resample places source frame `f` at the fraction `f / frames` of
            // the clip's authored tick span, whatever the tempo does in
            // between. So the retained window's own fractions of that span name
            // its end points in the source directly, and the flattened leaf
            // reads exactly what the untrimmed leaf reads over the same ticks.
            // Elapsed samples answer a different question and would put a
            // tempo-ramped leaf at the wrong audio.
            const auto authored_span = pending.child.duration().value;
            if (authored_span <= 0 || media->frame_count == 0)
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.child.id()}};
            const auto span = static_cast<long double>(authored_span);
            const auto frames = static_cast<long double>(media->frame_count);
            const auto begin = static_cast<long double>(pending.left_trim) / span * frames;
            const auto end =
                static_cast<long double>(authored_span - pending.right_trim) / span * frames;
            // A window this narrow retains no source frame to read, and a
            // source range that does not advance is not one the renderer's
            // phase map can divide by.
            if (!(begin >= 0.0L) || !(end > begin) || !(end <= frames))
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.child.id()}};
            source_frame_offset = static_cast<double>(begin);
            source_frame_phase_end = static_cast<double>(end);
            if (!(source_frame_phase_end > source_frame_offset))
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.child.id()}};
        } else if (auto* media = std::get_if<timeline::MediaRef>(&content);
                   media && pending.left_trim > 0 &&
                   pending.child.time_conform() == timeline::TimeConform::None) {
            // Only a leaf that plays its source at native rate can answer a
            // trim by moving into the reference. A stretched leaf's artifact is
            // keyed to the whole reference, so narrowing it here would change
            // what gets stretched rather than which part of the result is
            // heard; the artifact window does that instead.
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
        // A fade is measured from the clip's own edge, and a trim moves that
        // edge, so the retained fade is the authored one minus the trim rather
        // than the same ramp viewed part-way through. That is the ordinary
        // answer for a clip dragged shorter, and it keeps a nested leaf and a
        // hand-flattened one identical. A trim that swallows the whole fade
        // leaves none, and neither fade may outlast the clip that carries it.
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
        if (pending.composed_gain != 1.0f) {
            const auto composed = pending.composed_gain * playback.gain_linear;
            // Both factors are finite and non-negative, but their product can
            // still leave float range, and an infinite gain is not a louder
            // clip — it is an unrenderable one.
            if (!std::isfinite(composed))
                return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                       pending.child.id()}};
            playback.gain_linear = composed;
        }
        auto flattened = timeline::Clip::create(pending.generated_id, pending.target_start,
                                                pending.target_duration, std::move(content),
                                                playback, pending.child.time_conform());
        if (!flattened)
            return {.error = SequenceLoweringError{CompileErrorCode::InvalidStructure,
                                                   pending.child.id()}};
        return append(std::move(flattened).value(), pending.child.id(), source_frame_offset,
                      pending.context_sequence_id, pending.clipped_start, pending.pad_left,
                      pending.pad_right, std::move(pending.placement_fades), pending.left_trim,
                      pending.child.duration(), source_frame_phase_end);
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
