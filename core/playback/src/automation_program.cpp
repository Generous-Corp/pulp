#include <pulp/playback/automation_program.hpp>

#include <atomic>
#include <limits>
#include <optional>
#include <utility>

namespace pulp::playback {
namespace {

struct CompiledKnot {
    timebase::TickPosition tick;
    timebase::SamplePosition sample;
    float value = 0.0f;
    timeline::AutomationInterpolation interpolation = timeline::AutomationInterpolation::Continuous;
    float curvature = 0.0f;
};

runtime::Result<std::shared_ptr<const AutomationProgram>, AutomationProgramError>
fail(AutomationProgramErrorCode code, timeline::ItemId lane) {
    return runtime::Err(AutomationProgramError{code, lane});
}

AutomationProgramSegment make_segment(const CompiledKnot& start, const CompiledKnot& end) noexcept {
    return {start.tick,  end.tick,  start.sample,        end.sample,
            start.value, end.value, start.interpolation, start.curvature};
}

std::optional<AutomationProgramInstanceToken> next_instance_token() noexcept {
    static std::atomic<std::uint64_t> last_issued{0};
    auto current = last_issued.load(std::memory_order_relaxed);
    for (;;) {
        if (current == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        const auto next = current + 1u;
        if (last_issued.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                              std::memory_order_relaxed))
            return AutomationProgramInstanceToken{next};
    }
}

} // namespace

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
    if (generation == 0)
        return fail(AutomationProgramErrorCode::InvalidGeneration, lane.id());
    if (!tempo_map)
        return fail(AutomationProgramErrorCode::MissingTempoMap, lane.id());
    const auto* target = std::get_if<timeline::DeviceParameterTarget>(&lane.target());
    if (!target)
        return fail(AutomationProgramErrorCode::UnsupportedTarget, lane.id());

    std::vector<CompiledKnot> knots;
    knots.reserve(lane.curve().points().size());
    for (const auto& point : lane.curve().points()) {
        knots.push_back({point.position, tempo_map->ticks_to_samples(point.position), point.value,
                         point.interpolation, point.curvature});
    }

    std::vector<AutomationProgramSegment> segments;
    if (!knots.empty()) {
        segments.reserve(knots.size());
        for (std::size_t index = 1; index < knots.size(); ++index)
            segments.push_back(make_segment(knots[index - 1], knots[index]));
        segments.push_back(make_segment(knots.back(), knots.back()));
    }

    const float leading_value =
        lane.curve().points().empty() ? 0.0f : lane.curve().points().front().value;
    const auto instance_token = next_instance_token();
    if (!instance_token)
        return fail(AutomationProgramErrorCode::InstanceTokenExhausted, lane.id());
    return runtime::Ok(std::shared_ptr<const AutomationProgram>(new AutomationProgram(
        generation, *instance_token, lane.id(), *target, std::move(tempo_map), std::move(segments),
        leading_value)));
}

} // namespace pulp::playback
