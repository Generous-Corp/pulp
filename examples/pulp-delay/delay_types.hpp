#pragma once

#include <array>
#include <cstdint>

namespace pulp::examples::delay {

enum class Character : std::uint8_t {
    clean = 0,
    vintage,
    tape,
    bbd,
};

enum class OffsetMode : std::uint8_t {
    ratio = 0,
    milliseconds,
};

enum class Routing : std::uint8_t {
    mono = 0,
    stereo,
    ping_pong,
};

inline constexpr std::array<const char*, 11> kDivisionLabels = {
    "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1",
};

} // namespace pulp::examples::delay
