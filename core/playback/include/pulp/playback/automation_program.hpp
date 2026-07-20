#pragma once

#include <pulp/playback/program_identity.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/automation_lane.hpp>

#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::playback {

namespace detail {
class AutomationProgramCompiler;
}

struct AutomationProgramInstanceToken {
    std::uint64_t value = 0;
    constexpr auto operator<=>(const AutomationProgramInstanceToken&) const = default;
};

enum class AutomationProgramErrorCode : std::uint8_t {
    InvalidGeneration,
    MissingTempoMap,
    UnsupportedTarget,
    InstanceTokenExhausted,
};

struct AutomationProgramError {
    AutomationProgramErrorCode code = AutomationProgramErrorCode::InvalidGeneration;
    timeline::ItemId lane;
};

struct AutomationProgramSegment {
    timebase::TickPosition start_tick;
    timebase::TickPosition end_tick;
    timebase::SamplePosition start_sample;
    timebase::SamplePosition end_sample;
    float start_value = 0.0f;
    float end_value = 0.0f;
    timeline::AutomationInterpolation interpolation = timeline::AutomationInterpolation::Continuous;
    float curvature = 0.0f;

    constexpr bool operator==(const AutomationProgramSegment&) const = default;
};

/// Immutable tick- and sample-domain form of one automation lane. Every
/// non-empty program ends with a zero-length terminal segment, so the final
/// authored value remains unambiguous after sample-domain knot collisions and
/// clamped boundary values need no second point-storage representation.
class AutomationProgram {
  public:
    static runtime::Result<std::shared_ptr<const AutomationProgram>, AutomationProgramError>
    compile(const timeline::AutomationLane& lane,
            std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
            ProgramGeneration generation);

    ProgramGeneration generation() const noexcept {
        return generation_;
    }
    AutomationProgramInstanceToken instance_token() const noexcept {
        return instance_token_;
    }
    timeline::ItemId lane_id() const noexcept {
        return lane_id_;
    }
    timeline::DeviceParameterTarget target() const noexcept {
        return target_;
    }
    const timebase::CompiledTempoMap& tempo_map() const noexcept {
        return *tempo_map_;
    }
    const std::shared_ptr<const timebase::CompiledTempoMap>& tempo_map_owner() const noexcept {
        return tempo_map_;
    }
    std::span<const AutomationProgramSegment> segments() const noexcept {
        return segments_;
    }
    float leading_value() const noexcept {
        return leading_value_;
    }
    bool empty() const noexcept {
        return segments_.empty();
    }

  private:
    friend class detail::AutomationProgramCompiler;
    AutomationProgram(ProgramGeneration generation, AutomationProgramInstanceToken instance_token,
                      timeline::ItemId lane_id,
                      timeline::DeviceParameterTarget target,
                      std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                      std::vector<AutomationProgramSegment> segments, float leading_value) noexcept;

    ProgramGeneration generation_ = 0;
    AutomationProgramInstanceToken instance_token_;
    timeline::ItemId lane_id_;
    timeline::DeviceParameterTarget target_;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map_;
    std::vector<AutomationProgramSegment> segments_;
    float leading_value_ = 0.0f;
};

} // namespace pulp::playback
