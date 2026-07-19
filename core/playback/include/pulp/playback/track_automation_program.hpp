#pragma once

#include <pulp/playback/automation_program.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/automation_lane.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::playback {

enum class TrackAutomationProgramErrorCode : std::uint8_t {
    InvalidTrackId,
    MissingTempoMap,
    MissingProgram,
    InvalidLaneId,
    TempoMapMismatch,
    DuplicateLane,
    DuplicateTarget,
};

struct TrackAutomationProgramError {
    TrackAutomationProgramErrorCode code = TrackAutomationProgramErrorCode::InvalidTrackId;
    timeline::ItemId track;
    timeline::ItemId lane;
    timeline::ItemId related_lane;
    timeline::DeviceParameterTarget target;
};

/// Immutable compiler-supplied grouping of one track's compiled automation
/// lanes. This type validates the grouping but does not prove document
/// attachment. Programs are canonicalized by lane identity while retaining
/// their exact owners, so a rebuilt aggregate can reuse unchanged lane
/// generations and instance tokens.
class TrackAutomationProgram {
  public:
    static runtime::Result<std::shared_ptr<const TrackAutomationProgram>,
                           TrackAutomationProgramError>
    create(timeline::ItemId track_id,
           std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
           std::vector<std::shared_ptr<const AutomationProgram>> programs);

    timeline::ItemId track_id() const noexcept {
        return track_id_;
    }
    const timebase::CompiledTempoMap& tempo_map() const noexcept {
        return *tempo_map_;
    }
    const std::shared_ptr<const timebase::CompiledTempoMap>& tempo_map_owner() const noexcept {
        return tempo_map_;
    }
    std::span<const std::shared_ptr<const AutomationProgram>> programs() const noexcept {
        return programs_;
    }
    const AutomationProgram* find_lane(timeline::ItemId lane_id) const noexcept;

  private:
    TrackAutomationProgram(timeline::ItemId track_id,
                           std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                           std::vector<std::shared_ptr<const AutomationProgram>> programs) noexcept;

    timeline::ItemId track_id_;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map_;
    std::vector<std::shared_ptr<const AutomationProgram>> programs_;
};

} // namespace pulp::playback
