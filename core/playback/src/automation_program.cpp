#include <pulp/playback/automation_program.hpp>

#include "automation_program_compiler.hpp"

#include <utility>

namespace pulp::playback {

AutomationProgram::AutomationProgram(ProgramGeneration generation,
                                     AutomationProgramInstanceToken instance_token,
                                     timeline::ItemId lane_id,
                                     timeline::DeviceParameterTarget target,
                                     std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                                     std::vector<AutomationProgramSegment> segments,
                                     float leading_value) noexcept
    : generation_(generation), instance_token_(instance_token), lane_id_(lane_id), target_(target),
      tempo_map_(std::move(tempo_map)), segments_(std::move(segments)),
      leading_value_(leading_value) {}

runtime::Result<std::shared_ptr<const AutomationProgram>, AutomationProgramError>
AutomationProgram::compile(const timeline::AutomationLane& lane,
                           std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                           ProgramGeneration generation) {
    detail::AutomationProgramCompiler compiler;
    compiler.reset(lane, std::move(tempo_map), generation);
    for (;;) {
        auto step = compiler.step();
        if (!step)
            return runtime::Err(step.error());
        if (step.value() == detail::AutomationProgramCompileStatus::Complete)
            return runtime::Ok(compiler.take_result());
    }
}

} // namespace pulp::playback
