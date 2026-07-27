#pragma once

#include <pulp/playback/program_identity.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/automation_lane.hpp>

#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
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
    const timeline::AutomationTarget& target() const noexcept {
        return target_;
    }
    /// The device parameter this lane drives, or none when it drives a control
    /// the owning track holds itself. Device delivery only ever concerns the
    /// former; returning a pointer keeps that split explicit at every consumer.
    const timeline::DeviceParameterTarget* device_target() const noexcept {
        return std::get_if<timeline::DeviceParameterTarget>(&target_);
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
                      timeline::ItemId lane_id, timeline::AutomationTarget target,
                      std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                      std::vector<AutomationProgramSegment> segments, float leading_value) noexcept;

    ProgramGeneration generation_ = 0;
    AutomationProgramInstanceToken instance_token_;
    timeline::ItemId lane_id_;
    timeline::AutomationTarget target_;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map_;
    std::vector<AutomationProgramSegment> segments_;
    float leading_value_ = 0.0f;
};

/// Selects the segment governing an absolute timeline sample. A cold call
/// searches; a warm one advances forward from `segment_index`, which is the
/// amortized O(1) path monotonic playback takes. Never called on an empty
/// program.
std::size_t select_automation_segment(const AutomationProgram& program,
                                      timebase::SamplePosition sample, bool cold,
                                      std::size_t segment_index) noexcept;

/// The authored value of one already-selected segment at an exact tick. The
/// sample is needed only to recognise a zero-length terminal segment. Every
/// consumer that needs a curve value at a position goes through here, so device
/// delivery and track-mixer application can never drift apart.
float evaluate_automation_segment(const AutomationProgram& program, std::size_t segment_index,
                                  timebase::SamplePosition sample,
                                  timebase::TickPosition tick) noexcept;

} // namespace pulp::playback
