#pragma once

#include "delay_types.hpp"

#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>

#include <cstddef>

namespace pulp::examples::delay {

enum Param : state::ParamID {
    kTime = 1,
    kFeedback = 2,
    kCharacter = 3,
    kMix = 4,
    kCharacterAmount = 5,
    kDiffusion = 6,
    kSync = 7,
    kDivision = 8,
    kLink = 9,
    kOffsetMode = 10,
    kTimeOffset = 11,
    kOffsetMs = 12,
    kTimeRight = 13,
    kDivisionRight = 14,
    kRouting = 15,
    kCrossfeed = 16,
    kDuck = 17,
    kLowCut = 18,
    kHighCut = 19,
    kLowCutResonance = 20,
    kHighCutResonance = 21,
    kModRate = 22,
    kModDepth = 23,
    kFreeze = 24,
    kReverse = 25,
};

inline constexpr std::size_t kParameterCount = 25;
static_assert(kReverse == kParameterCount);

void define_delay_parameters(state::StateStore& store);

Character character_from_param(float value) noexcept;
OffsetMode offset_mode_from_param(float value) noexcept;
Routing routing_from_param(float value) noexcept;
int division_index_from_param(float value) noexcept;

} // namespace pulp::examples::delay
