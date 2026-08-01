#include "sequence_preflight.hpp"

#include "sequence_content_lowerer.hpp"

#include <algorithm>
#include <variant>

namespace pulp::playback {

runtime::Result<std::vector<timeline::ItemId>, CompileError>
collect_reachable_asset_ids(const timeline::Project& project, const timeline::Sequence& root,
                            const timebase::CompiledTempoMap& tempo_map,
                            std::uint64_t max_expanded_note_events,
                            std::uint64_t max_expanded_clips) {
    SequenceContentLowerer lowerer{project, tempo_map, max_expanded_note_events,
                                   max_expanded_clips};
    std::vector<LoweredClip> lowered;
    std::vector<timeline::ItemId> result;
    const auto add_media = [&](const timeline::MediaRef& media) {
        result.push_back(media.asset_id);
    };
    const auto add_clip_media = [&](const timeline::Clip& clip) {
        std::visit(timeline::ClipContentCases{
                       [](const timeline::EmptyContent&) {},
                       [&](const timeline::MediaRef& media) { add_media(media); },
                       [](const timeline::MidiContent&) {},
                       [](const timeline::RegisteredContent&) {},
                       [](const timeline::OpaqueContent&) {},
                       [](const timeline::SequenceRef&) {},
                   },
                   clip.content());
    };
    for (const auto& track : root.tracks()) {
        const auto begun = lowerer.begin_track(track, lowered);
        if (begun.error)
            return runtime::Err(CompileError{begun.error->code, begun.error->item});
        if (begun.plan.kind == TrackContentKind::Freeze) {
            add_media(begun.plan.freeze->media);
            continue;
        }
        if (begun.plan.kind == TrackContentKind::ActiveTake) {
            for (const auto& segment : begun.plan.active_take_lane->comp_segments()) {
                const auto* take = begun.plan.active_take_lane->find_take(segment.take_id);
                if (!take)
                    return runtime::Err(
                        CompileError{CompileErrorCode::InvalidStructure, segment.take_id});
                add_media(take->media());
            }
            continue;
        }

        while (true) {
            const auto step = lowerer.step();
            if (step.error)
                return runtime::Err(CompileError{step.error->code, step.error->item});
            if (step.complete)
                break;
        }
        for (const auto& clip : lowered)
            add_clip_media(clip.clip);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return runtime::Ok(std::move(result));
}

} // namespace pulp::playback
