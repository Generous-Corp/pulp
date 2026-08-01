#include <pulp/interchange/census.hpp>

#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/automation_lane.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <variant>

namespace pulp::interchange {
namespace {

using timeline::ItemId;

constexpr std::uint8_t velocity_16_to_7(std::uint16_t value) noexcept {
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) * 127u + 0x7fffu) /
                                     0xffffu);
}

constexpr std::uint16_t velocity_7_to_16(std::uint8_t value) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(value) * 0xffffu + 63u) /
                                      127u);
}

void record_clip(ConceptCensus& out, const timeline::Project& project,
                 const timeline::Clip& clip, const CensusLimits& limits) {
    const ItemId id = clip.id();
    out.record(clip.time_anchor() == timeline::ClipTimeAnchor::Musical ? Concept::ClipMusical
                                                                       : Concept::ClipAbsolute,
               id, limits);

    std::visit(
        timeline::ClipContentCases{
            [&](const timeline::EmptyContent&) { out.record(Concept::ClipEmpty, id, limits); },
            [&](const timeline::MediaRef& media) {
                out.record(Concept::ClipMedia, id, limits);
                const timeline::MediaAsset* asset = project.find_asset(media.asset_id);
                // Project validation guarantees the asset exists. Keep the
                // defensive null check here so an unresolved reference can
                // never be mistaken for a lossless full-asset window.
                if (!asset || media.source_start.value != 0 ||
                    media.frame_count != asset->frame_count)
                    out.record(Concept::ClipMediaWindow, id, limits);
            },
            [&](const timeline::MidiContent& notes) {
                out.record(Concept::ClipNote, id, limits);
                if (std::any_of(notes.notes().begin(), notes.notes().end(), [](const auto& note) {
                        const auto encoded = velocity_16_to_7(note.velocity);
                        return encoded == 0 || velocity_7_to_16(encoded) != note.velocity;
                    }))
                    out.record(Concept::ClipNoteVelocityQuantized, id, limits);
                // Modifiers decide whether a note sounds at all, so a format
                // that cannot carry them loses audible content. A seed with no
                // modifiers still selects a replay, so it counts too. Recording
                // both is what keeps an export's loss manifest from claiming
                // nothing was lost.
                if (!notes.modifiers().empty() || notes.modifier_seed() != 0)
                    out.record(Concept::ClipNoteModifier, id, limits);
            },
            [&](const timeline::RegisteredContent&) {
                out.record(Concept::ContentRegistered, id, limits);
            },
            [&](const timeline::OpaqueContent&) { out.record(Concept::ContentOpaque, id, limits); },
            [&](const timeline::SequenceRef&) {
                out.record(Concept::SequenceNested, id, limits);
            },
        },
        clip.content());

    const timeline::ClipPlaybackProperties playback = clip.playback_properties();
    if (playback.gain_linear != 1.0f)
        out.record(Concept::ClipGain, id, limits);
    if (playback.fade_in_duration != 0 || playback.fade_out_duration != 0)
        out.record(Concept::ClipFades, id, limits);
}

void record_track(ConceptCensus& out, const timeline::Project& project,
                  const timeline::Track& track, const CensusLimits& limits) {
    const ItemId id = track.id();
    // Every track in the model holds its clips directly; grouping is a concept
    // the document cannot express yet, so it is never recorded here.
    out.record(Concept::TrackFlat, id, limits);

    for (const timeline::Clip& clip : track.clips())
        record_clip(out, project, clip, limits);

    for (const timeline::DevicePlacement& device : track.device_chain())
        out.record(Concept::DevicePlacement, device.id, limits);

    const timeline::TrackMixer mixer = track.mixer();
    if (mixer.gain_linear != 1.0f)
        out.record(Concept::MixerTrackGain, id, limits);
    if (mixer.pan != 0.0f)
        out.record(Concept::MixerTrackPan, id, limits);

    for (const timeline::AutomationLane& lane : track.automation_lanes()) {
        std::visit(timeline::AutomationTargetCases{
                       [&](const timeline::DeviceParameterTarget&) {
                           out.record(Concept::AutomationDeviceParam, lane.id(), limits);
                       },
                       [&](const timeline::TrackMixerTarget& target) {
                           out.record(target.parameter == timeline::TrackMixerParameter::Gain
                                          ? Concept::AutomationTrackGain
                                          : Concept::AutomationTrackPan,
                                      lane.id(), limits);
                       },
                   },
                   lane.target());
    }

    for (const timeline::TakeLane& lane : track.take_lanes()) {
        out.record(Concept::TakeLane, lane.id(), limits);
        if (!lane.comp_segments().empty())
            out.record(Concept::TakeComp, lane.id(), limits);
    }

    if (track.freeze().has_value())
        out.record(Concept::TrackFreeze, id, limits);
}

} // namespace

bool ConceptCensus::contains(Concept concept_value) const noexcept {
    return entries_[static_cast<std::size_t>(concept_value)].count != 0;
}

std::uint64_t ConceptCensus::count(Concept concept_value) const noexcept {
    return entries_[static_cast<std::size_t>(concept_value)].count;
}

std::span<const timeline::ItemId> ConceptCensus::owners(Concept concept_value) const noexcept {
    return entries_[static_cast<std::size_t>(concept_value)].owners;
}

std::vector<Concept> ConceptCensus::present() const {
    std::vector<Concept> found;
    for (std::size_t index = 0; index < kConceptCount; ++index) {
        if (entries_[index].count != 0)
            found.push_back(static_cast<Concept>(index));
    }
    return found;
}

bool ConceptCensus::empty() const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.count != 0)
            return false;
    }
    return true;
}

void ConceptCensus::record(Concept concept_value, timeline::ItemId owner,
                           const CensusLimits& limits) {
    Entry& entry = entries_[static_cast<std::size_t>(concept_value)];
    ++entry.count;
    if (entry.owners.size() < limits.max_owners_per_concept)
        entry.owners.push_back(owner);
}

ConceptCensus census(const timeline::Project& project, const CensusLimits& limits) {
    ConceptCensus out;
    const ItemId project_id = project.id();

    for (const timeline::MediaAsset& asset : project.assets()) {
        // Every asset in the model is sealed to a hash of its bytes; the storage
        // policy is what decides whether the bytes travel with the document.
        out.record(Concept::AssetSealedHash, asset.id, limits);
        out.record(asset.storage_policy == timeline::AssetStoragePolicy::External
                       ? Concept::AssetReferencedMedia
                       : Concept::AssetEmbeddedMedia,
                   asset.id, limits);
    }

    for (const timeline::Sequence& sequence : project.sequences()) {
        // Context the document states but a format has no lane for is data an
        // export drops. Recording it here is what keeps the loss manifest from
        // claiming nothing was lost.
        if (!sequence.chord_scale_lane().empty())
            out.record(Concept::ContextChordScale, sequence.id(), limits);
        if (!sequence.groove().is_canonical_default())
            out.record(Concept::ContextGroove, sequence.id(), limits);
        // Markers and regions share one concept: its vocabulary entry is "a
        // named point or range on the timeline", so a span is the same concept
        // as a point rather than a second atom. Both are named locations a
        // format either carries or drops.
        for (const timeline::SequenceMarker& marker : sequence.markers())
            out.record(Concept::Marker, marker.id, limits);
        for (const timeline::SequenceRegion& region : sequence.regions())
            out.record(Concept::Marker, region.id, limits);
        for (const timeline::Scene& scene : sequence.scenes())
            out.record(Concept::ClipLaunch, scene.id, limits);
        for (const timeline::Track& track : sequence.tracks())
            record_track(out, project, track, limits);
    }
    if (project.sequences().size() > 1)
        out.record(Concept::SequenceMultiple, project_id, limits);
    // The wall-clock origin the document's zero represents. A format without it
    // opens the session at zero, silently moving every timecode reference.
    if (project.session_start())
        out.record(Concept::TimecodeOrigin, project_id, limits);

    // One point is a constant governing the whole document; more than one is a
    // map, and formats that carry only a constant lose the difference.
    out.record(project.tempo_map().points().size() > 1 ? Concept::TempoMap : Concept::TempoSingle,
               project_id, limits);
    for (const timebase::TempoPoint& point : project.tempo_map().points()) {
        if (point.curve_to_next != timebase::TempoCurve::Constant)
            out.record(Concept::TempoRamp, project_id, limits);
        const auto microseconds = std::llround(60'000'000.0 / point.bpm);
        if (microseconds <= 0 || microseconds > 0xff'ffff ||
            60'000'000.0 / static_cast<double>(microseconds) != point.bpm)
            out.record(Concept::TempoValueQuantized, project_id, limits);
    }
    out.record(project.meter_map().points().size() > 1 ? Concept::MeterMap : Concept::MeterSingle,
               project_id, limits);

    return out;
}

} // namespace pulp::interchange
