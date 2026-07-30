#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>

namespace pulp::signal::detail {

/// Converts fractional synthesis-hop advances into integer hops without an
/// ever-growing floating-point absolute position. Keeping only the bounded
/// residual preserves sub-sample increments at arbitrarily large stream
/// positions. Integer correction debt records the difference between the ideal
/// rounded advance and the hop that could actually be emitted after clamping.
class FractionalSynthesisHopAccumulator {
public:
    int advance(double exact_hop, int minimum_hop, int maximum_hop) noexcept {
        assert(std::isfinite(exact_hop) && exact_hop > 0.0
               && exact_hop <= static_cast<double>(maximum_hop));
        assert(minimum_hop > 0 && minimum_hop <= maximum_hop);
        const double accumulated = residual_ + exact_hop;
        const auto desired = static_cast<std::int64_t>(std::llround(accumulated));
        residual_ = accumulated - static_cast<double>(desired);

        // Compare before adding so even a debt at an int64 boundary cannot make
        // candidate-hop arithmetic overflow. Because the exact hop never
        // exceeds the admitted upper bound, only the lower clamp can create
        // debt and that debt is always non-positive.
        assert(correction_debt_ <= 0);
        const auto lower_threshold = static_cast<std::int64_t>(minimum_hop) - desired;
        int emitted = minimum_hop;
        if (correction_debt_ >= lower_threshold)
            emitted = static_cast<int>(correction_debt_ + desired);
        assert(emitted >= minimum_hop && emitted <= maximum_hop);

        const auto adjustment = desired - emitted;
        const bool underflows = adjustment < 0
            && correction_debt_ < std::numeric_limits<std::int64_t>::min() - adjustment;
        assert(!underflows);
        // FiniteStretch admission proves cumulative emitted output fits int64.
        // Because desired cumulative advance is non-negative, negative debt can
        // never exceed that same emitted-output bound. Fail closed if a caller
        // violates the shared counter invariant; never saturate and lose debt.
        if (underflows)
            std::terminate();
        correction_debt_ += adjustment;
        assert(correction_debt_ <= 0);
        return emitted;
    }

    void reset() noexcept {
        residual_ = 0.0;
        correction_debt_ = 0;
    }
    double residual() const noexcept { return residual_; }
    std::int64_t correction_debt() const noexcept { return correction_debt_; }

private:
    double residual_ = 0.0;
    std::int64_t correction_debt_ = 0;
};

} // namespace pulp::signal::detail
