#include <pulp/playback/automation_program.hpp>

#include "automation_program_compiler.hpp"

#include <pulp/timeline/automation_curve.hpp>

#include <algorithm>
#include <utility>

namespace pulp::playback {

AutomationProgram::AutomationProgram(ProgramGeneration generation,
                                     AutomationProgramInstanceToken instance_token,
                                     timeline::ItemId lane_id,
                                     timeline::AutomationTarget target,
                                     std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                                     std::vector<AutomationProgramSegment> segments,
                                     float leading_value) noexcept
    : generation_(generation), instance_token_(instance_token), lane_id_(lane_id), target_(std::move(target)),
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

std::size_t select_automation_segment(const AutomationProgram& program,
                                      timebase::SamplePosition sample, bool cold,
                                      std::size_t segment_index) noexcept {
    const auto segments = program.segments();
    if (cold) {
        const auto found = std::upper_bound(
            segments.begin(), segments.end(), sample,
            [](timebase::SamplePosition value, const AutomationProgramSegment& segment) {
                return value < segment.end_sample;
            });
        return found == segments.end() ? segments.size() - 1u
                                       : static_cast<std::size_t>(found - segments.begin());
    }
    while (segment_index + 1u < segments.size() &&
           sample >= segments[segment_index].end_sample) {
        ++segment_index;
    }
    return segment_index;
}

float evaluate_automation_segment(const AutomationProgram& program, std::size_t segment_index,
                                  timebase::SamplePosition sample,
                                  timebase::TickPosition tick) noexcept {
    const auto segments = program.segments();
    if (sample < segments.front().start_sample)
        return program.leading_value();
    const auto& segment = segments[segment_index];
    if (segment.start_sample == segment.end_sample)
        return segment.end_value;
    if (segment.start_tick == segment.end_tick || tick <= segment.start_tick)
        return segment.start_value;
    if (tick >= segment.end_tick)
        return segment.end_value;
    if (segment.interpolation == timeline::AutomationInterpolation::Hold)
        return segment.start_value;
    return timeline::evaluate_continuous_automation_segment(tick, segment.start_tick,
                                                            segment.end_tick, segment.start_value,
                                                            segment.end_value, segment.curvature);
}

} // namespace pulp::playback
