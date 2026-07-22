#include "automation_program_compiler.hpp"

#include <atomic>
#include <limits>
#include <optional>
#include <utility>

namespace pulp::playback::detail {
namespace {

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

runtime::Result<AutomationProgramCompileStatus, AutomationProgramError>
fail(AutomationProgramErrorCode code, timeline::ItemId lane) {
    return runtime::Err(AutomationProgramError{code, lane});
}

} // namespace

AutomationProgramSegment
AutomationProgramCompiler::make_segment(const CompiledKnot& start,
                                        const CompiledKnot& end) noexcept {
    return {start.tick,  end.tick,  start.sample,        end.sample,
            start.value, end.value, start.interpolation, start.curvature};
}

void AutomationProgramCompiler::reset(
    const timeline::AutomationLane& lane,
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
    ProgramGeneration generation) {
    lane_ = &lane;
    tempo_map_ = std::move(tempo_map);
    generation_ = generation;
    stage_ = Stage::Validate;
    index_ = 0;
    target_ = {};
    knots_.clear();
    knots_.reserve(lane.curve().points().size());
    segments_.clear();
    segments_.reserve(lane.curve().points().size());
    result_.reset();
}

runtime::Result<AutomationProgramCompileStatus, AutomationProgramError>
AutomationProgramCompiler::step() {
    if (stage_ == Stage::Validate) {
        if (generation_ == 0)
            return fail(AutomationProgramErrorCode::InvalidGeneration, lane_->id());
        if (!tempo_map_)
            return fail(AutomationProgramErrorCode::MissingTempoMap, lane_->id());
        const auto* target = std::get_if<timeline::DeviceParameterTarget>(&lane_->target());
        if (!target)
            return fail(AutomationProgramErrorCode::UnsupportedTarget, lane_->id());
        target_ = *target;
        stage_ = Stage::Knots;
        return runtime::Ok(AutomationProgramCompileStatus::Pending);
    }
    if (stage_ == Stage::Knots) {
        if (index_ < lane_->curve().points().size()) {
            const auto& point = lane_->curve().points()[index_++];
            knots_.push_back({point.position, tempo_map_->ticks_to_samples(point.position),
                              point.value, point.interpolation, point.curvature});
            return runtime::Ok(AutomationProgramCompileStatus::Pending);
        }
        index_ = 1;
        stage_ = Stage::Segments;
        return runtime::Ok(AutomationProgramCompileStatus::Pending);
    }
    if (stage_ == Stage::Segments) {
        if (index_ < knots_.size()) {
            segments_.push_back(make_segment(knots_[index_ - 1], knots_[index_]));
            ++index_;
            return runtime::Ok(AutomationProgramCompileStatus::Pending);
        }
        if (!knots_.empty())
            segments_.push_back(make_segment(knots_.back(), knots_.back()));
        stage_ = Stage::Finalize;
        return runtime::Ok(AutomationProgramCompileStatus::Pending);
    }
    if (stage_ == Stage::Finalize) {
        const auto token = next_instance_token();
        if (!token)
            return fail(AutomationProgramErrorCode::InstanceTokenExhausted, lane_->id());
        const auto leading_value =
            lane_->curve().points().empty() ? 0.0f : lane_->curve().points().front().value;
        result_ = std::shared_ptr<const AutomationProgram>(new AutomationProgram(
            generation_, *token, lane_->id(), target_, tempo_map_, std::move(segments_),
            leading_value));
        stage_ = Stage::Complete;
        return runtime::Ok(AutomationProgramCompileStatus::Complete);
    }
    return runtime::Ok(AutomationProgramCompileStatus::Complete);
}

std::shared_ptr<const AutomationProgram> AutomationProgramCompiler::take_result() noexcept {
    lane_ = nullptr;
    tempo_map_.reset();
    generation_ = 0;
    index_ = 0;
    return std::move(result_);
}

} // namespace pulp::playback::detail
