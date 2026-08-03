#include <pulp/timeline_agent_view/agent_view.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <bit>
#include <string_view>
#include <tuple>
#include <utility>

namespace pulp::timeline_agent_view {
namespace {

// There is no public per-entity canonical serializer. Variant widening is a
// compile failure here. Scalar-field additions are not mechanically visible to
// C++; they remain an explicit kAgentViewVersion/hash-schema review obligation.
static_assert(timeline::kClipContentAlternativeCount == 6);
static_assert(timeline::kAutomationTargetAlternativeCount == 2);

using timeline::Clip;
using timeline::ClipTimeAnchor;
using timeline::DirtyFlags;
using timeline::DocumentRevision;
using timeline::ItemId;

struct CanonicalBudget {
    std::size_t remaining = 0;
};

class CanonicalRows {
  public:
    CanonicalRows(std::string_view domain, CanonicalBudget& budget) : budget_(budget) {
        text("pulp.agent-view.v1");
        text(domain);
    }
    void u64(std::uint64_t value) { text(std::to_string(value)); }
    void i64(std::int64_t value) { text(std::to_string(value)); }
    void boolean(bool value) { u64(value ? 1 : 0); }
    void f32(float value) { u64(std::bit_cast<std::uint32_t>(value)); }
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void id(ItemId value) { u64(value.value); }
    void text(std::string_view value) {
        if (!ok_)
            return;
        const auto prefix = std::to_string(value.size());
        if (prefix.size() > budget_.remaining || value.size() > budget_.remaining - prefix.size() ||
            2 > budget_.remaining - prefix.size() - value.size()) {
            ok_ = false;
            return;
        }
        budget_.remaining -= prefix.size() + value.size() + 2;
        bytes_.append(prefix);
        bytes_.push_back(':');
        bytes_.append(value);
        bytes_.push_back(';');
    }
    std::optional<std::string> finish() const {
        if (!ok_)
            return std::nullopt;
        return runtime::sha256_hex(bytes_);
    }

  private:
    CanonicalBudget& budget_;
    std::string bytes_;
    bool ok_ = true;
};

void rate(CanonicalRows& out, timebase::RationalRate value) {
    out.u64(value.numerator);
    out.u64(value.denominator);
}

void media_ref(CanonicalRows& out, const timeline::MediaRef& value) {
    out.id(value.asset_id);
    out.i64(value.source_start.value);
    out.u64(value.frame_count);
}

void schema(CanonicalRows& out, const timeline::SchemaIdentity& value) {
    out.text(value.type_name);
    out.u64(value.version);
}

void append_clip_content(CanonicalRows& out, const timeline::ClipContent& content) {
    std::visit(timeline::ClipContentCases{
                   [&](const timeline::EmptyContent&) { out.text("empty"); },
                   [&](const timeline::MediaRef& media) {
                       out.text("media");
                       media_ref(out, media);
                   },
                   [&](const timeline::MidiContent& midi) {
                       out.text("midi");
                       out.u64(midi.modifier_seed());
                       out.u64(midi.notes().size());
                       for (const auto& note : midi.notes()) {
                           out.id(note.id);
                           out.i64(note.start.value);
                           out.i64(note.duration.value);
                           out.u64(note.velocity);
                           out.u64(note.pitch);
                           out.u64(note.channel);
                       }
                       out.u64(midi.modifiers().size());
                       for (const auto& modifier : midi.modifiers()) {
                           out.id(modifier.note_id);
                           out.u64(modifier.probability);
                           out.u64(modifier.condition_period);
                           out.u64(modifier.condition_offset);
                           out.u64(modifier.ratchet_count);
                           out.u64(static_cast<std::uint8_t>(modifier.condition));
                       }
                       out.u64(midi.lanes().size());
                       for (const auto& lane : midi.lanes()) {
                           out.id(lane.id);
                           out.u64(lane.address.group);
                           out.u64(lane.address.channel);
                           out.u64(lane.address.status);
                           out.u64(lane.address.bank);
                           out.u64(lane.address.index);
                           out.u64(lane.points.size());
                           for (const auto& point : lane.points) {
                               out.id(point.id);
                               out.i64(point.position.value);
                               out.u64(point.value);
                           }
                       }
                   },
                   [&](const timeline::RegisteredContent& registered) {
                       out.text("registered");
                       schema(out, registered.schema());
                       out.text(registered.canonical_payload_json());
                   },
                   [&](const timeline::OpaqueContent& opaque) {
                       out.text("opaque");
                       schema(out, opaque.schema());
                       out.text(opaque.raw_json());
                   },
                   [&](const timeline::SequenceRef& sequence) {
                       out.text("sequence-ref");
                       out.id(sequence.sequence_id);
                       out.i64(sequence.source_start.value);
                   }},
               content);
}

std::size_t clip_omitted_count(const Clip& clip) {
    const auto* midi = std::get_if<timeline::MidiContent>(&clip.content());
    if (!midi)
        return 0;
    std::size_t count = midi->notes().size() + midi->lanes().size();
    for (const auto& lane : midi->lanes())
        count += lane.points.size();
    return count;
}

std::optional<std::string> clip_omitted_hash(const Clip& clip, CanonicalBudget& budget) {
    CanonicalRows out("clip-omitted", budget);
    if (clip.time_anchor() == ClipTimeAnchor::Absolute)
        rate(out, clip.absolute_sample_rate());
    const auto playback = clip.playback_properties();
    out.f32(playback.gain_linear);
    out.u64(playback.fade_in_duration);
    out.u64(playback.fade_out_duration);
    out.u64(static_cast<std::uint8_t>(playback.fade_shape));
    out.u64(static_cast<std::uint8_t>(clip.time_conform()));
    append_clip_content(out, clip.content());
    return out.finish();
}

std::optional<ClipSummary> summarize_clip(const Clip& clip, ItemId sequence_id, ItemId track_id,
                                          CanonicalBudget& budget) {
    ClipSummary result;
    result.sequence_id = sequence_id;
    result.track_id = track_id;
    result.id = clip.id();
    result.anchor = clip.time_anchor();
    if (result.anchor == ClipTimeAnchor::Musical) {
        result.start = clip.start().value;
        result.duration = static_cast<std::uint64_t>(clip.duration().value);
    } else {
        result.start = clip.absolute_start().value;
        result.duration = clip.absolute_duration_samples();
    }
    auto omitted_hash = clip_omitted_hash(clip, budget);
    if (!omitted_hash)
        return std::nullopt;
    result.omitted = {clip_omitted_count(clip), std::move(*omitted_hash)};
    CanonicalRows content("clip", budget);
    content.id(clip.id());
    content.u64(static_cast<std::uint8_t>(clip.time_anchor()));
    content.i64(result.start);
    content.u64(result.duration);
    content.text(result.omitted.sha256);
    auto content_hash = content.finish();
    if (!content_hash)
        return std::nullopt;
    result.content_sha256 = std::move(*content_hash);
    return result;
}

void append_automation_target(CanonicalRows& out, const timeline::AutomationTarget& target) {
    std::visit(timeline::AutomationTargetCases{
                   [&](const timeline::DeviceParameterTarget& value) {
                       out.text("device");
                       out.id(value.device_placement_id);
                       out.u64(value.param_id);
                   },
                   [&](const timeline::TrackMixerTarget& value) {
                       out.text("mixer");
                       out.u64(static_cast<std::uint8_t>(value.parameter));
                   }},
               target);
}

std::size_t track_omitted_count(const timeline::Track& track) {
    std::size_t count = track.device_chain().size() + track.automation_lanes().size() +
                        track.take_lanes().size();
    for (const auto& lane : track.automation_lanes())
        count += lane.curve().points().size();
    for (const auto& lane : track.take_lanes())
        count += lane.takes().size() + lane.comp_segments().size();
    return count;
}

std::optional<std::string> track_omitted_hash(const timeline::Track& track,
                                              CanonicalBudget& budget) {
    CanonicalRows out("track-omitted", budget);
    out.boolean(track.record_armed());
    out.id(track.active_take_lane_id());
    out.f32(track.mixer().gain_linear);
    out.f32(track.mixer().pan);
    out.boolean(track.freeze().has_value());
    if (track.freeze()) {
        media_ref(out, track.freeze()->media);
        out.i64(track.freeze()->placement_start.value);
        rate(out, track.freeze()->sample_rate);
        out.text(track.freeze()->render_plan_hash.to_hex());
    }
    out.u64(track.device_chain().size());
    for (const auto& device : track.device_chain())
        out.id(device.id);
    out.u64(track.automation_lanes().size());
    for (const auto& lane : track.automation_lanes()) {
        out.id(lane.id());
        append_automation_target(out, lane.target());
        out.u64(lane.curve().points().size());
        for (const auto& point : lane.curve().points()) {
            out.id(point.id);
            out.i64(point.position.value);
            out.f32(point.value);
            out.u64(static_cast<std::uint8_t>(point.interpolation));
            out.f32(point.curvature);
        }
    }
    out.u64(track.take_lanes().size());
    for (const auto& lane : track.take_lanes()) {
        out.id(lane.id());
        out.text(lane.name());
        out.u64(lane.takes().size());
        for (const auto& take : lane.takes()) {
            out.id(take.id());
            media_ref(out, take.media());
            out.i64(take.placement_start().value);
            rate(out, take.sample_rate());
        }
        out.u64(lane.comp_segments().size());
        for (const auto& segment : lane.comp_segments()) {
            out.id(segment.take_id);
            out.i64(segment.range.start.value);
            out.u64(segment.range.sample_count);
            rate(out, segment.range.sample_rate);
        }
    }
    return out.finish();
}

std::size_t sequence_omitted_count(const timeline::Sequence& sequence) {
    std::size_t count = sequence.markers().size() + sequence.regions().size() +
                        sequence.chord_scale_lane().events().size() +
                        sequence.groove().steps().size() + sequence.scenes().size();
    for (const auto& scene : sequence.scenes())
        count += scene.slots.size();
    return count;
}

std::optional<std::string> sequence_omitted_hash(const timeline::Sequence& sequence,
                                                 CanonicalBudget& budget) {
    CanonicalRows out("sequence-omitted", budget);
    out.boolean(sequence.duration().has_value());
    if (sequence.duration())
        out.i64(sequence.duration()->value);
    out.boolean(sequence.absolute_duration().has_value());
    if (sequence.absolute_duration()) {
        out.u64(sequence.absolute_duration()->sample_count);
        rate(out, sequence.absolute_duration()->sample_rate);
    }
    out.u64(sequence.track_order().size());
    for (const auto track_id : sequence.track_order())
        out.id(track_id);
    out.u64(sequence.markers().size());
    for (const auto& marker : sequence.markers()) {
        out.id(marker.id);
        out.text(marker.name);
        out.i64(marker.position.value);
        out.boolean(marker.color.has_value());
        if (marker.color)
            out.u64(*marker.color);
    }
    out.u64(sequence.regions().size());
    for (const auto& region : sequence.regions()) {
        out.id(region.id);
        out.text(region.name);
        out.i64(region.position.value);
        out.i64(region.duration.value);
        out.boolean(region.color.has_value());
        if (region.color)
            out.u64(*region.color);
    }
    out.u64(sequence.chord_scale_lane().events().size());
    for (const auto& event : sequence.chord_scale_lane().events()) {
        out.i64(event.position.value);
        out.u64(static_cast<std::uint8_t>(event.chord_quality));
        out.u64(event.chord_root);
        out.u64(static_cast<std::uint8_t>(event.scale_mode));
        out.u64(event.scale_root);
    }
    const auto& groove = sequence.groove();
    out.text(groove.name());
    out.i64(groove.swing_grid().value);
    out.u64(groove.swing().numerator);
    out.u64(groove.swing().denominator);
    out.i64(groove.step().value);
    out.i64(groove.timing_strength());
    out.i64(groove.velocity_strength());
    out.u64(groove.steps().size());
    for (const auto& step : groove.steps()) {
        out.i64(step.timing_offset.value);
        out.i64(step.velocity_scale);
    }
    out.u64(sequence.scenes().size());
    for (const auto& scene : sequence.scenes()) {
        out.id(scene.id);
        out.text(scene.name);
        out.u64(scene.slots.size());
        for (const auto& slot : scene.slots) {
            out.id(slot.id);
            out.id(slot.clip_id);
            out.i64(slot.launch_quantize.grid.value);
            out.i64(slot.launch_quantize.phase.value);
            out.u64(slot.follow.choice_count);
            out.u64(slot.follow.repetitions);
            out.i64(slot.follow.grid.value);
            for (const auto& action : slot.follow.active()) {
                out.u64(static_cast<std::uint8_t>(action.kind));
                out.id(action.target);
                out.u64(action.weight);
            }
        }
    }
    return out.finish();
}

std::optional<std::string> project_omitted_hash(const timeline::Project& project,
                                                CanonicalBudget& budget) {
    CanonicalRows out("project-omitted", budget);
    out.u64(project.next_item_id());
    out.id(project.root_sequence_id());
    out.u64(project.tempo_map().points().size());
    for (const auto& point : project.tempo_map().points()) {
        out.i64(point.tick.value);
        out.f64(point.bpm);
        out.u64(static_cast<std::uint8_t>(point.curve_to_next));
    }
    out.u64(project.meter_map().points().size());
    for (const auto& point : project.meter_map().points()) {
        out.i64(point.tick.value);
        out.i64(point.signature.numerator);
        out.i64(point.signature.denominator);
    }
    out.boolean(project.session_start().has_value());
    if (project.session_start()) {
        out.i64(project.session_start()->start.value);
        rate(out, project.session_start()->sample_rate);
    }
    out.u64(project.assets().size());
    for (const auto& asset : project.assets()) {
        out.id(asset.id);
        out.text(asset.name);
        out.u64(asset.frame_count);
        rate(out, asset.sample_rate);
        out.text(asset.content_hash.to_hex());
        out.u64(static_cast<std::uint8_t>(asset.storage_policy));
        out.u64(asset.locators.size());
        for (const auto& locator : asset.locators) {
            out.u64(static_cast<std::uint8_t>(locator.kind));
            out.text(locator.hint);
        }
        out.u64(asset.representations.size());
        for (const auto& representation : asset.representations) {
            out.text(representation.role);
            out.text(representation.content_hash.to_hex());
            out.u64(static_cast<std::uint8_t>(representation.storage_policy));
            out.u64(representation.locators.size());
            for (const auto& locator : representation.locators) {
                out.u64(static_cast<std::uint8_t>(locator.kind));
                out.text(locator.hint);
            }
        }
        out.boolean(asset.loop_info.has_value());
        if (asset.loop_info) {
            const auto& loop = *asset.loop_info;
            out.boolean(loop.musical_length.has_value());
            if (loop.musical_length)
                out.i64(loop.musical_length->value);
            out.u64(loop.meter.numerator);
            out.u64(loop.meter.denominator);
            out.boolean(loop.one_shot);
            out.boolean(loop.root_note.has_value());
            if (loop.root_note)
                out.u64(*loop.root_note);
            out.boolean(loop.active_range.has_value());
            if (loop.active_range) {
                out.u64(loop.active_range->start_frame);
                out.u64(loop.active_range->end_frame);
            }
            out.u64(loop.points.size());
            for (const auto& point : loop.points) {
                out.u64(point.frame);
                out.u64(static_cast<std::uint8_t>(point.kind));
            }
            out.u64(loop.tags.size());
            for (const auto& tag : loop.tags)
                out.text(tag);
        }
    }
    return out.finish();
}

bool charge(std::size_t& work, std::size_t amount, std::size_t limit) {
    if (amount > limit - work)
        return false;
    work += amount;
    return true;
}

std::optional<timeline::ProjectSnapshotCounts>
census(const timeline::Project& project, std::size_t limit) {
    timeline::ProjectSnapshotCounts result;
    std::size_t work = 1;
    if (limit == 0 || !charge(work, project.assets().size(), limit) ||
        !charge(work, project.sequences().size(), limit))
        return std::nullopt;
    result.assets = project.assets().size();
    result.sequences = project.sequences().size();
    for (const auto& sequence : project.sequences()) {
        if (!charge(work, sequence.tracks().size(), limit) ||
            !charge(work, sequence.markers().size(), limit) ||
            !charge(work, sequence.regions().size(), limit) ||
            !charge(work, sequence.chord_scale_lane().events().size(), limit) ||
            !charge(work, sequence.groove().steps().size(), limit) ||
            !charge(work, sequence.scenes().size(), limit))
            return std::nullopt;
        result.tracks += sequence.tracks().size();
        result.markers += sequence.markers().size();
        result.regions += sequence.regions().size();
        result.chord_scale_events += sequence.chord_scale_lane().events().size();
        result.groove_steps += sequence.groove().steps().size();
        result.scenes += sequence.scenes().size();
        for (const auto& scene : sequence.scenes()) {
            if (!charge(work, scene.slots.size(), limit))
                return std::nullopt;
            result.slots += scene.slots.size();
        }
        for (const auto& track : sequence.tracks()) {
            if (!charge(work, track.clips().size(), limit) ||
                !charge(work, track.device_chain().size(), limit) ||
                !charge(work, track.automation_lanes().size(), limit) ||
                !charge(work, track.take_lanes().size(), limit))
                return std::nullopt;
            result.clips += track.clips().size();
            result.device_placements += track.device_chain().size();
            result.automation_lanes += track.automation_lanes().size();
            result.take_lanes += track.take_lanes().size();
            for (const auto& lane : track.automation_lanes()) {
                if (!charge(work, lane.curve().points().size(), limit))
                    return std::nullopt;
                result.automation_points += lane.curve().points().size();
            }
            for (const auto& lane : track.take_lanes()) {
                if (!charge(work, lane.takes().size(), limit) ||
                    !charge(work, lane.comp_segments().size(), limit))
                    return std::nullopt;
                result.takes += lane.takes().size();
                result.take_comp_segments += lane.comp_segments().size();
            }
            for (const auto& clip : track.clips()) {
                const auto* midi = std::get_if<timeline::MidiContent>(&clip.content());
                if (!midi)
                    continue;
                if (!charge(work, midi->notes().size(), limit) ||
                    !charge(work, midi->lanes().size(), limit))
                    return std::nullopt;
                result.notes += midi->notes().size();
                result.midi_lanes += midi->lanes().size();
                for (const auto& lane : midi->lanes()) {
                    if (!charge(work, lane.points.size(), limit))
                        return std::nullopt;
                    result.midi_lane_points += lane.points.size();
                }
            }
        }
    }
    return result;
}

std::size_t omitted_total(const timeline::ProjectSnapshotCounts& c) {
    return c.assets + c.notes + c.device_placements + c.automation_lanes +
           c.automation_points + c.take_lanes + c.takes + c.take_comp_segments + c.markers +
           c.regions + c.scenes + c.slots + c.chord_scale_events + c.groove_steps +
           c.midi_lanes + c.midi_lane_points;
}

Error stale(DocumentRevision expected, DocumentRevision actual) {
    return {ErrorCode::StaleRevision, expected, actual, {}};
}

DirtyFlags combine(DirtyFlags lhs, DirtyFlags rhs) {
    return static_cast<DirtyFlags>(static_cast<std::uint16_t>(lhs) |
                                   static_cast<std::uint16_t>(rhs));
}

bool has_flag(DirtyFlags value, DirtyFlags flag) {
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) != 0;
}

} // namespace

runtime::Result<AgentView, Error> AgentView::create(timeline::DocumentView view, Limits limits) {
    if (!view.snapshot)
        return runtime::Err(Error{ErrorCode::InvalidSnapshot, {}, view.revision, {}});
    if (limits.max_outline_items == 0 || limits.max_census_items == 0 ||
        limits.max_page_items == 0 ||
        limits.max_canonical_bytes == 0)
        return runtime::Err(Error{ErrorCode::LimitExceeded, {}, view.revision, {}});
    const auto counted = census(*view.snapshot, limits.max_census_items);
    if (!counted)
        return runtime::Err(Error{ErrorCode::LimitExceeded, {}, view.revision, view.snapshot->id()});
    const auto counts = *counted;
    if (counts.sequences > limits.max_outline_items - 1 ||
        counts.tracks > limits.max_outline_items - 1 - counts.sequences ||
        counts.clips > limits.max_outline_items - 1 - counts.sequences - counts.tracks)
        return runtime::Err(Error{ErrorCode::LimitExceeded, {}, view.revision, view.snapshot->id()});
    const auto explicit_count = std::size_t{1} + counts.sequences + counts.tracks + counts.clips;
    if (explicit_count > limits.max_outline_items)
        return runtime::Err(Error{ErrorCode::LimitExceeded, {}, view.revision, view.snapshot->id()});
    return runtime::Ok(AgentView(std::move(view), limits, counts));
}

runtime::Result<Outline, Error> AgentView::outline(DocumentRevision expected_revision) const {
    if (expected_revision != view_.revision)
        return runtime::Err(stale(expected_revision, view_.revision));
    const auto explicit_count = std::size_t{1} + counts_.sequences + counts_.tracks + counts_.clips;
    if (explicit_count > limits_.max_outline_items)
        return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision, view_.revision, {}});

    Outline result;
    CanonicalBudget budget{limits_.max_canonical_bytes};
    result.revision = view_.revision;
    result.project_id = view_.snapshot->id();
    result.project_name = view_.snapshot->name();
    result.census = counts_;
    result.explicit_item_count = explicit_count;
    auto project_omitted = project_omitted_hash(*view_.snapshot, budget);
    if (!project_omitted)
        return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision, view_.revision,
                                  view_.snapshot->id()});
    result.omitted = {counts_.assets, std::move(*project_omitted)};
    for (const auto& sequence : view_.snapshot->sequences()) {
        SequenceSummary sequence_summary;
        sequence_summary.id = sequence.id();
        sequence_summary.name = sequence.name();
        auto sequence_omitted = sequence_omitted_hash(sequence, budget);
        if (!sequence_omitted)
            return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision, view_.revision,
                                      sequence.id()});
        sequence_summary.omitted = {sequence_omitted_count(sequence),
                                    std::move(*sequence_omitted)};
        for (const auto& track : sequence.tracks()) {
            TrackSummary track_summary;
            track_summary.sequence_id = sequence.id();
            track_summary.id = track.id();
            track_summary.name = track.name();
            auto track_omitted = track_omitted_hash(track, budget);
            if (!track_omitted)
                return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision,
                                          view_.revision, track.id()});
            track_summary.omitted = {track_omitted_count(track), std::move(*track_omitted)};
            for (const auto& clip : track.clips()) {
                auto clip_summary = summarize_clip(clip, sequence.id(), track.id(), budget);
                if (!clip_summary)
                    return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision,
                                              view_.revision, clip.id()});
                track_summary.clips.push_back(std::move(*clip_summary));
            }
            CanonicalRows track_content("track", budget);
            track_content.id(track.id());
            track_content.text(track.name());
            track_content.text(track_summary.omitted.sha256);
            for (const auto& clip : track_summary.clips)
                track_content.text(clip.content_sha256);
            auto hash = track_content.finish();
            if (!hash)
                return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision,
                                          view_.revision, track.id()});
            track_summary.content_sha256 = std::move(*hash);
            sequence_summary.tracks.push_back(std::move(track_summary));
        }
        CanonicalRows sequence_content("sequence", budget);
        sequence_content.id(sequence.id());
        sequence_content.text(sequence.name());
        sequence_content.text(sequence_summary.omitted.sha256);
        for (const auto& track : sequence_summary.tracks)
            sequence_content.text(track.content_sha256);
        auto hash = sequence_content.finish();
        if (!hash)
            return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision, view_.revision,
                                      sequence.id()});
        sequence_summary.content_sha256 = std::move(*hash);
        result.sequences.push_back(std::move(sequence_summary));
    }
    CanonicalRows project_content("project", budget);
    project_content.id(result.project_id);
    project_content.text(result.project_name);
    project_content.text(result.omitted.sha256);
    for (const auto& sequence : result.sequences)
        project_content.text(sequence.content_sha256);
    auto project_hash = project_content.finish();
    if (!project_hash)
        return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision, view_.revision,
                                  result.project_id});
    result.content_sha256 = std::move(*project_hash);

    std::size_t partitioned = result.omitted.count;
    for (const auto& sequence : result.sequences) {
        partitioned += sequence.omitted.count;
        for (const auto& track : sequence.tracks) {
            partitioned += track.omitted.count;
            for (const auto& clip : track.clips)
                partitioned += clip.omitted.count;
        }
    }
    if (partitioned != omitted_total(counts_))
        return runtime::Err(Error{ErrorCode::InvalidSnapshot, expected_revision, view_.revision,
                                  result.project_id});
    return runtime::Ok(std::move(result));
}

runtime::Result<RegionPage, Error> AgentView::region(const RegionRequest& request) const {
    if (request.expected_revision != view_.revision)
        return runtime::Err(stale(request.expected_revision, view_.revision));
    if (request.start >= request.end)
        return runtime::Err(Error{ErrorCode::InvalidRange, request.expected_revision,
                                  view_.revision, request.sequence_id});
    if (request.limit == 0 || request.limit > limits_.max_page_items)
        return runtime::Err(Error{ErrorCode::LimitExceeded, request.expected_revision,
                                  view_.revision, request.sequence_id});
    const auto* sequence = view_.snapshot->find_sequence(request.sequence_id);
    if (!sequence)
        return runtime::Err(Error{ErrorCode::MissingSequence, request.expected_revision,
                                  view_.revision, request.sequence_id});
    std::vector<ClipSummary> candidates;
    CanonicalBudget budget{limits_.max_canonical_bytes};
    for (const auto& track : sequence->tracks()) {
        for (const auto& clip : track.clips()) {
            if (clip.time_anchor() != request.anchor)
                continue;
            const auto position = request.anchor == ClipTimeAnchor::Musical
                                      ? clip.start().value
                                      : clip.absolute_start().value;
            if (position < request.start || position >= request.end)
                continue;
            if (candidates.size() >= limits_.max_outline_items)
                return runtime::Err(Error{ErrorCode::LimitExceeded, request.expected_revision,
                                          view_.revision, request.sequence_id});
            auto summary = summarize_clip(clip, sequence->id(), track.id(), budget);
            if (!summary)
                return runtime::Err(Error{ErrorCode::LimitExceeded, request.expected_revision,
                                          view_.revision, clip.id()});
            candidates.push_back(std::move(*summary));
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.start, lhs.id.value) < std::tie(rhs.start, rhs.id.value);
    });
    std::size_t begin = 0;
    if (request.after) {
        const auto& cursor = *request.after;
        if (cursor.version != kAgentViewVersion || cursor.revision != view_.revision ||
            cursor.sequence_id != request.sequence_id || cursor.anchor != request.anchor ||
            cursor.window_start != request.start || cursor.window_end != request.end)
            return runtime::Err(Error{ErrorCode::InvalidCursor, request.expected_revision,
                                      view_.revision, cursor.clip_id});
        const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto& item) {
            return item.start == cursor.start && item.id == cursor.clip_id;
        });
        if (found == candidates.end())
            return runtime::Err(Error{ErrorCode::InvalidCursor, request.expected_revision,
                                      view_.revision, cursor.clip_id});
        begin = static_cast<std::size_t>(std::distance(candidates.begin(), found)) + 1;
    }
    RegionPage page;
    page.revision = view_.revision;
    const auto page_count = std::min(candidates.size() - begin, request.limit);
    const auto end = begin + page_count;
    page.items.insert(page.items.end(), candidates.begin() + static_cast<std::ptrdiff_t>(begin),
                      candidates.begin() + static_cast<std::ptrdiff_t>(end));
    if (end < candidates.size() && !page.items.empty()) {
        const auto& last = page.items.back();
        page.next = RegionCursor{kAgentViewVersion, view_.revision, request.sequence_id,
                                 request.anchor, request.start, request.end, last.start, last.id};
    }
    return runtime::Ok(std::move(page));
}

runtime::Result<OutlineDiff, Error>
AgentView::diff(DocumentRevision expected_revision, DirtyRevisionRange revisions,
                const timeline::DirtySet& dirty) const {
    if (expected_revision != view_.revision)
        return runtime::Err(stale(expected_revision, view_.revision));
    if (revisions.after != view_.revision || revisions.after.value == 0 ||
        revisions.before.value != revisions.after.value - 1)
        return runtime::Err(Error{ErrorCode::InvalidProvenance, expected_revision,
                                  view_.revision, {}});
    if (dirty.items().size() > limits_.max_outline_items ||
        dirty.contexts().size() > limits_.max_outline_items - dirty.items().size())
        return runtime::Err(Error{ErrorCode::LimitExceeded, expected_revision,
                                  view_.revision, {}});
    OutlineDiff result;
    result.revision = view_.revision;
    result.changes.reserve(dirty.items().size() + dirty.contexts().size());
    for (const auto& item : dirty.items()) {
        OutlineChange change{OutlineKind::Project, item.owner_sequence, item.owner_track,
                             item.item, item.flags};
        const auto location = view_.snapshot->locate(item.item);
        if (!location || location->sequence_id != item.owner_sequence ||
            location->track_id != item.owner_track ||
            (!location->active && !has_flag(item.flags, DirtyFlags::Removed)))
            return runtime::Err(Error{ErrorCode::InvalidDirtySet, expected_revision,
                                      view_.revision, item.item});
        {
            switch (location->kind) {
            case timeline::ItemKind::Project:
            case timeline::ItemKind::Asset:
                change.kind = OutlineKind::Project;
                change.item_id = view_.snapshot->id();
                break;
            case timeline::ItemKind::Sequence:
            case timeline::ItemKind::Marker:
            case timeline::ItemKind::Region:
            case timeline::ItemKind::Scene:
            case timeline::ItemKind::Slot:
                change.kind = OutlineKind::Sequence;
                change.sequence_id = location->sequence_id.valid() ? location->sequence_id
                                                                   : item.item;
                change.track_id = {};
                change.item_id = change.sequence_id;
                break;
            case timeline::ItemKind::Track:
            case timeline::ItemKind::DevicePlacement:
            case timeline::ItemKind::AutomationLane:
            case timeline::ItemKind::AutomationPoint:
            case timeline::ItemKind::TakeLane:
            case timeline::ItemKind::Take:
                change.kind = OutlineKind::Track;
                change.track_id = location->track_id.valid() ? location->track_id : item.item;
                change.item_id = change.track_id;
                break;
            case timeline::ItemKind::Clip:
            case timeline::ItemKind::Note:
            case timeline::ItemKind::MidiLane:
            case timeline::ItemKind::MidiLanePoint:
                change.kind = OutlineKind::Clip;
                change.track_id = location->track_id;
                change.item_id = location->clip_id.valid() ? location->clip_id : item.item;
                break;
            }
        }
        result.changes.push_back(change);
    }
    for (const auto& context : dirty.contexts()) {
        if (!context.owner_sequence.valid() ||
            view_.snapshot->find_sequence(context.owner_sequence) == nullptr)
            return runtime::Err(Error{ErrorCode::InvalidDirtySet, expected_revision,
                                      view_.revision, context.owner_sequence});
        result.changes.push_back({OutlineKind::Sequence, context.owner_sequence, {},
                                  context.owner_sequence, DirtyFlags::Context});
    }
    std::sort(result.changes.begin(), result.changes.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.kind, lhs.sequence_id.value, lhs.track_id.value, lhs.item_id.value) <
               std::tie(rhs.kind, rhs.sequence_id.value, rhs.track_id.value, rhs.item_id.value);
    });
    std::vector<OutlineChange> merged;
    for (const auto& change : result.changes) {
        if (!merged.empty() && merged.back().kind == change.kind &&
            merged.back().sequence_id == change.sequence_id &&
            merged.back().track_id == change.track_id && merged.back().item_id == change.item_id)
            merged.back().flags = combine(merged.back().flags, change.flags);
        else
            merged.push_back(change);
    }
    result.changes = std::move(merged);
    return runtime::Ok(std::move(result));
}

} // namespace pulp::timeline_agent_view
