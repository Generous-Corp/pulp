#pragma once

#include "crossfade.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <type_traits>

namespace pulp::signal {

/// Compute adjacent-path constant-power weights for a position in [0, N-1].
/// All unselected weights are zero and sum(weight^2) is one. A non-finite
/// position selects path zero. Empty output is rejected.
template <typename SampleType>
inline bool nway_constant_power_gains(SampleType position, std::span<SampleType> gains) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (gains.empty())
        return false;
    std::fill(gains.begin(), gains.end(), SampleType{0});
    if (!std::isfinite(position))
        position = SampleType{0};
    if (position <= SampleType{0}) {
        gains.front() = SampleType{1};
        return true;
    }
    const auto final_position = static_cast<SampleType>(gains.size() - 1);
    if (position >= final_position) {
        gains.back() = SampleType{1};
        return true;
    }
    const auto lower = static_cast<std::size_t>(position);
    const auto fraction = position - static_cast<SampleType>(lower);
    crossfade_gains(fraction, CrossfadeGainLaw::EqualPower, gains[lower], gains[lower + 1]);
    return true;
}

} // namespace pulp::signal
