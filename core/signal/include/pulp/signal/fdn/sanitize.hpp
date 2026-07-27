#pragma once

// Scalar recovery shared by recursive FDN stages. This stays independent of
// any particular filter topology so delay, diffusion, and envelope code do not
// acquire a biquad dependency merely to stop NaN/Inf propagation.

#include <cmath>

namespace pulp::signal::fdn {

template <typename T>
inline T finite_or_zero(T value) {
    return std::isfinite(static_cast<double>(value)) ? value : T{0};
}

}  // namespace pulp::signal::fdn
