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
            [&](const NoteContent& notes) {
                for (const auto& note : notes.notes())
                    visitor(ModelOwnedIdentity{
                        .id = note.id, .kind = ItemKind::Note, .track = track, .clip = clip.id()});
            },
            [](const RegisteredContent&) {},
            [](const OpaqueContent&) {},
            [](const SequenceRef&) {},
        },
        clip.content());
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
    for (const auto& track : sequence.tracks()) {
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
}

} // namespace pulp::timeline::detail
