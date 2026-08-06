#pragma once

#include <pulp/timeline/model.hpp>

namespace pulp::timeline::detail {

struct ModelOwnedIdentity {
    ItemId id;
    ItemKind kind = ItemKind::Project;
    ItemId track;
    ItemId clip;
    ItemId lane;
};

template <typename Visitor>
void visit_clip_owned_identities(const Clip& clip, ItemId track, Visitor&& visitor) {
    visitor(ModelOwnedIdentity{
        .id = clip.id(), .kind = ItemKind::Clip, .track = track, .clip = clip.id()});
    std::visit(
        ClipContentCases{
            [](const EmptyContent&) {},
            [](const MediaRef&) {},
            [&](const MidiContent& notes) {
                for (const auto& note : notes.notes())
                    visitor(ModelOwnedIdentity{
                        .id = note.id, .kind = ItemKind::Note, .track = track, .clip = clip.id()});
                for (const auto& lane : notes.lanes()) {
                    visitor(ModelOwnedIdentity{.id = lane.id,
                                               .kind = ItemKind::MidiLane,
                                               .track = track,
                                               .clip = clip.id(),
                                               .lane = lane.id});
                    for (const auto& point : lane.points)
                        visitor(ModelOwnedIdentity{.id = point.id,
                                                   .kind = ItemKind::MidiLanePoint,
                                                   .track = track,
                                                   .clip = clip.id(),
                                                   .lane = lane.id});
                }
            },
            [](const RegisteredContent&) {},
            [](const OpaqueContent&) {},
            [](const SequenceRef&) {},
        },
        clip.content());
}

// The authoritative enumeration of everything one track owns. A track owns
// eleven identity kinds across four levels, two of them parented by a lane
// rather than by the track, so any caller that needs the owned set — a whole
// sequence, or one track lifted out of it — walks this one traversal instead
// of restating the list.
template <typename Visitor>
void visit_track_owned_identities(const Track& track, Visitor&& visitor) {
    visitor(ModelOwnedIdentity{.id = track.id(), .kind = ItemKind::Track, .track = track.id()});
    for (const auto& device : track.device_chain())
        visitor(ModelOwnedIdentity{
            .id = device.id, .kind = ItemKind::DevicePlacement, .track = track.id()});
    for (const auto& clip : track.clips())
        visit_clip_owned_identities(clip, track.id(), visitor);
    for (const auto& lane : track.automation_lanes()) {
        visitor(ModelOwnedIdentity{.id = lane.id(),
                                   .kind = ItemKind::AutomationLane,
                                   .track = track.id(),
                                   .lane = lane.id()});
        for (const auto& point : lane.curve().points())
            visitor(ModelOwnedIdentity{.id = point.id,
                                       .kind = ItemKind::AutomationPoint,
                                       .track = track.id(),
                                       .lane = lane.id()});
    }
    for (const auto& modulator : track.modulators())
        visitor(ModelOwnedIdentity{
            .id = modulator.id, .kind = ItemKind::Modulator, .track = track.id()});
    for (const auto& macro : track.macros())
        visitor(ModelOwnedIdentity{
            .id = macro.id, .kind = ItemKind::MacroControl, .track = track.id()});
    for (const auto& route : track.modulation_routes())
        visitor(ModelOwnedIdentity{
            .id = route.id, .kind = ItemKind::ModulationRoute, .track = track.id()});
    for (const auto& lane : track.take_lanes()) {
        visitor(ModelOwnedIdentity{.id = lane.id(),
                                   .kind = ItemKind::TakeLane,
                                   .track = track.id(),
                                   .lane = lane.id()});
        for (const auto& take : lane.takes())
            visitor(ModelOwnedIdentity{.id = take.id(),
                                       .kind = ItemKind::Take,
                                       .track = track.id(),
                                       .lane = lane.id()});
    }
}

template <typename Visitor>
void visit_sequence_owned_identities(const Sequence& sequence, Visitor&& visitor) {
    visitor(ModelOwnedIdentity{.id = sequence.id(), .kind = ItemKind::Sequence});
    for (const auto& marker : sequence.markers())
        visitor(ModelOwnedIdentity{.id = marker.id, .kind = ItemKind::Marker});
    for (const auto& region : sequence.regions())
        visitor(ModelOwnedIdentity{.id = region.id, .kind = ItemKind::Region});
    for (const auto& scene : sequence.scenes()) {
        visitor(ModelOwnedIdentity{.id = scene.id, .kind = ItemKind::Scene});
        for (const auto& slot : scene.slots)
            visitor(ModelOwnedIdentity{
                .id = slot.id, .kind = ItemKind::Slot, .lane = scene.id});
    }
    for (const auto& track : sequence.tracks())
        visit_track_owned_identities(track, visitor);
}

} // namespace pulp::timeline::detail
