#pragma once

#include "automation_program_compiler.hpp"
#include "budgeted_stable_merge.hpp"

#include <pulp/playback/program_identity.hpp>
#include <pulp/playback/track_automation_program.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <memory>
#include <vector>

namespace pulp::playback::detail {

struct CompiledTrackAutomation {
    std::vector<timeline::ItemId> ordered_device_placement_ids;
    std::shared_ptr<const TrackAutomationProgram> program;
};

struct TrackAutomationCompileError {
    timeline::ItemId item;
};

enum class TrackAutomationCompileStatus { Pending, Complete };

class TrackAutomationCompiler {
  public:
    void reset(const timeline::Track& track,
               std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
               ProgramGeneration generation);
    runtime::Result<TrackAutomationCompileStatus, TrackAutomationCompileError> step();
    CompiledTrackAutomation take_result() noexcept;

  private:
    enum class Stage {
        Placements,
        Lanes,
        SortLanes,
        ValidateLanes,
        PrepareTargets,
        SortTargets,
        ValidateTargets,
        Aggregate,
        Complete,
    };

    const timeline::Track* track_ = nullptr;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map_;
    ProgramGeneration generation_ = 0;
    Stage stage_ = Stage::Complete;
    std::size_t index_ = 0;
    CompiledTrackAutomation compiled_;
    std::vector<std::shared_ptr<const AutomationProgram>> lane_programs_;
    AutomationProgramCompiler lane_compiler_;
    bool lane_compiler_active_ = false;
    std::vector<std::shared_ptr<const AutomationProgram>> lane_merge_buffer_;
    BudgetedStableMergeState lane_merge_;
    std::vector<const AutomationProgram*> programs_by_target_;
    std::vector<const AutomationProgram*> target_merge_buffer_;
    BudgetedStableMergeState target_merge_;
};

} // namespace pulp::playback::detail
