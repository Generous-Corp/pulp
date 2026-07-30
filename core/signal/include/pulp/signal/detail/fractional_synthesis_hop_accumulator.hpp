#pragma once

#include <cassert>
#include <cmath>
#include <limits>

namespace pulp::signal::detail {

/// Converts fractional synthesis-hop advances into integer hops without an
/// ever-growing floating-point absolute position. Keeping only the bounded
/// residual preserves sub-sample increments at arbitrarily large stream
/// positions; a checked signed integer accumulator owns absolute position.
class FractionalSynthesisHopAccumulator {
public:
    int advance(double exact_hop) noexcept {
        assert(std::isfinite(exact_hop) && exact_hop > 0.0
               && exact_hop < static_cast<double>(std::numeric_limits<int>::max()));
        const double accumulated = residual_ + exact_hop;
        const auto emitted = static_cast<int>(std::llround(accumulated));
        residual_ = accumulated - static_cast<double>(emitted);
        return emitted;
    }

    void reset() noexcept { residual_ = 0.0; }
    double residual() const noexcept { return residual_; }

private:
    double residual_ = 0.0;
};

} // namespace pulp::signal::detail
