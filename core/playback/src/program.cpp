#include <pulp/playback/program.hpp>

#include <algorithm>
#include <limits>

namespace pulp::playback {

runtime::Result<ProgramGeneration, ProgramError>
next_program_generation(ProgramGeneration current) noexcept {
    if (current == std::numeric_limits<ProgramGeneration>::max())
        return runtime::Err(ProgramError{ProgramErrorCode::InvalidGeneration, {}});
    return runtime::Ok(current + 1);
}

TrackProgram::TrackProgram(timeline::ItemId id, ProgramGeneration generation,
                           ProviderSelectorProgram provider, RendererStatePolicy state_policy,
                           std::vector<timeline::ItemId> clip_ids) noexcept
    : id_(id), generation_(generation), provider_(provider), state_policy_(state_policy),
      clip_ids_(std::move(clip_ids)) {}

PlaybackProgram::PlaybackProgram(
    ProgramGeneration generation, std::uint64_t document_revision,
    timeline::ItemId project_id, timeline::ItemId sequence_id,
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
    std::vector<std::shared_ptr<const TrackProgram>> tracks) noexcept
    : generation_(generation), document_revision_(document_revision), project_id_(project_id),
      sequence_id_(sequence_id), tempo_map_(std::move(tempo_map)), tracks_(std::move(tracks)) {}

const std::shared_ptr<const TrackProgram>*
PlaybackProgram::find_track_owner(timeline::ItemId id) const noexcept {
    const auto found = std::lower_bound(tracks_.begin(), tracks_.end(), id,
        [](const auto& track, timeline::ItemId value) { return track->id() < value; });
    return found != tracks_.end() && (*found)->id() == id ? &*found : nullptr;
}

const TrackProgram* PlaybackProgram::find_track(timeline::ItemId id) const noexcept {
    const auto* owner = find_track_owner(id);
    return owner ? owner->get() : nullptr;
}

} // namespace pulp::playback
