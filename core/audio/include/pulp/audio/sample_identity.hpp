#pragma once

#include <cstdint>
#include <limits>

namespace pulp::audio {

inline constexpr std::uint32_t kInvalidSampleId =
    std::numeric_limits<std::uint32_t>::max();

}  // namespace pulp::audio

