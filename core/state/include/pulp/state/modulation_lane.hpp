#pragma once

#include <pulp/state/parameter.hpp>

#include <cstdint>
#include <string_view>

namespace pulp::state {

using ModulationSourceId = uint32_t;

enum class ModulationScope : uint8_t {
    Global,
    Voice,
    Note,
    GraphNode,
};

enum class ModulationRate : uint8_t {
    Control,
    Audio,
};

enum class ModulationMixMode : uint8_t {
    Add,
    Replace,
    Multiply,
};

struct ModulationSource {
    ModulationSourceId id = 0;
    ModulationScope scope = ModulationScope::Global;
    ModulationRate rate = ModulationRate::Control;
    std::string_view units;
};

struct ModulationTarget {
    ParamID param_id = 0;
    ModulationScope scope = ModulationScope::Global;
    ParamRate param_rate = ParamRate::ControlRate;
    bool modulatable = true;
    bool writable = true;
    std::string_view units;
};

struct ModulationLane {
    ModulationSource source;
    ModulationTarget target;
    ModulationMixMode mix = ModulationMixMode::Add;
    float depth = 1.0f;
};

enum class ModulationLaneRejectReason : uint8_t {
    None,
    InvalidSource,
    InvalidTarget,
    TargetNotWritable,
    TargetNotModulatable,
    ScopeMismatch,
    AudioSourceRequiresAudioTarget,
};

struct ModulationLaneValidation {
    bool accepted = false;
    ModulationLaneRejectReason reason = ModulationLaneRejectReason::None;
};

constexpr ModulationRate modulation_rate_for(ParamRate rate) noexcept {
    return rate == ParamRate::AudioRate
        ? ModulationRate::Audio
        : ModulationRate::Control;
}

constexpr bool modulation_scopes_compatible(ModulationScope source,
                                            ModulationScope target) noexcept {
    return source == target || source == ModulationScope::Global;
}

constexpr ModulationLaneValidation validate_modulation_lane(
    const ModulationLane& lane) noexcept {
    if (lane.source.id == 0) {
        return {false, ModulationLaneRejectReason::InvalidSource};
    }
    if (lane.target.param_id == 0) {
        return {false, ModulationLaneRejectReason::InvalidTarget};
    }
    if (!lane.target.writable) {
        return {false, ModulationLaneRejectReason::TargetNotWritable};
    }
    if (!lane.target.modulatable) {
        return {false, ModulationLaneRejectReason::TargetNotModulatable};
    }
    if (!modulation_scopes_compatible(lane.source.scope, lane.target.scope)) {
        return {false, ModulationLaneRejectReason::ScopeMismatch};
    }
    if (lane.source.rate == ModulationRate::Audio &&
        modulation_rate_for(lane.target.param_rate) != ModulationRate::Audio) {
        return {false, ModulationLaneRejectReason::AudioSourceRequiresAudioTarget};
    }
    return {true, ModulationLaneRejectReason::None};
}

} // namespace pulp::state
