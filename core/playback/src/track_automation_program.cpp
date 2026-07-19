#include <pulp/playback/track_automation_program.hpp>

#include <algorithm>
#include <utility>

namespace pulp::playback {
namespace {

runtime::Result<std::shared_ptr<const TrackAutomationProgram>, TrackAutomationProgramError>
fail(TrackAutomationProgramErrorCode code, timeline::ItemId track,
     timeline::ItemId lane = {}, timeline::ItemId related_lane = {},
     timeline::DeviceParameterTarget target = {}) {
    return runtime::Err(TrackAutomationProgramError{code, track, lane, related_lane, target});
}

bool target_less(const AutomationProgram* lhs, const AutomationProgram* rhs) noexcept {
    const auto lhs_target = lhs->target();
    const auto rhs_target = rhs->target();
    if (lhs_target.device_id != rhs_target.device_id)
        return lhs_target.device_id < rhs_target.device_id;
    if (lhs_target.param_id != rhs_target.param_id)
        return lhs_target.param_id < rhs_target.param_id;
    return lhs->lane_id() < rhs->lane_id();
}

bool lane_less(const std::shared_ptr<const AutomationProgram>& lhs,
               const std::shared_ptr<const AutomationProgram>& rhs) noexcept {
    if (lhs->lane_id() != rhs->lane_id())
        return lhs->lane_id() < rhs->lane_id();
    return target_less(lhs.get(), rhs.get());
}

} // namespace

TrackAutomationProgram::TrackAutomationProgram(
    timeline::ItemId track_id, std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
    std::vector<std::shared_ptr<const AutomationProgram>> programs) noexcept
    : track_id_(track_id), tempo_map_(std::move(tempo_map)), programs_(std::move(programs)) {}

runtime::Result<std::shared_ptr<const TrackAutomationProgram>, TrackAutomationProgramError>
TrackAutomationProgram::create(
    timeline::ItemId track_id, std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
    std::vector<std::shared_ptr<const AutomationProgram>> programs) {
    if (!track_id.valid())
        return fail(TrackAutomationProgramErrorCode::InvalidTrackId, track_id);
    if (!tempo_map)
        return fail(TrackAutomationProgramErrorCode::MissingTempoMap, track_id);

    for (const auto& program : programs) {
        if (!program)
            return fail(TrackAutomationProgramErrorCode::MissingProgram, track_id);
    }

    std::sort(programs.begin(), programs.end(), lane_less);
    for (const auto& program : programs) {
        if (!program->lane_id().valid() || program->lane_id() == track_id)
            return fail(TrackAutomationProgramErrorCode::InvalidLaneId, track_id,
                        program->lane_id());
    }
    for (const auto& program : programs) {
        if (program->tempo_map_owner().get() != tempo_map.get())
            return fail(TrackAutomationProgramErrorCode::TempoMapMismatch, track_id,
                        program->lane_id(), {}, program->target());
    }

    for (std::size_t index = 1; index < programs.size(); ++index) {
        if (programs[index - 1]->lane_id() == programs[index]->lane_id())
            return fail(TrackAutomationProgramErrorCode::DuplicateLane, track_id,
                        programs[index]->lane_id(), programs[index - 1]->lane_id(),
                        programs[index]->target());
    }

    std::vector<const AutomationProgram*> by_target;
    by_target.reserve(programs.size());
    for (const auto& program : programs)
        by_target.push_back(program.get());
    std::sort(by_target.begin(), by_target.end(), target_less);
    for (std::size_t index = 1; index < by_target.size(); ++index) {
        if (by_target[index - 1]->target() == by_target[index]->target())
            return fail(TrackAutomationProgramErrorCode::DuplicateTarget, track_id,
                        by_target[index]->lane_id(), by_target[index - 1]->lane_id(),
                        by_target[index]->target());
    }

    return runtime::Ok(std::shared_ptr<const TrackAutomationProgram>(new TrackAutomationProgram(
        track_id, std::move(tempo_map), std::move(programs))));
}

const AutomationProgram* TrackAutomationProgram::find_lane(timeline::ItemId lane_id) const noexcept {
    const auto found = std::lower_bound(
        programs_.begin(), programs_.end(), lane_id,
        [](const auto& program, timeline::ItemId id) { return program->lane_id() < id; });
    return found != programs_.end() && (*found)->lane_id() == lane_id ? found->get() : nullptr;
}

} // namespace pulp::playback
