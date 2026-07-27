#pragma once

// Sanitized recursive filter state shared by the FDN's coefficient-driven
// biquads. Recursive stages recover locally from non-finite input or state and
// snap their state words out of the denormal range as the tail decays.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/sanitize.hpp>

namespace pulp::signal::fdn {

class SanitizedBiquadState {
  public:
    void reset() {
        s1_ = 0.0;
        s2_ = 0.0;
    }

    double process(const BiquadCoefficientsT<double>& coefficients, double input) {
        const double in = finite_or_zero(input);
        const double output = finite_or_zero(coefficients.b0 * in + s1_);
        s1_ = finite_or_zero(snap_to_zero(coefficients.b1 * in - coefficients.a1 * output + s2_));
        s2_ = finite_or_zero(snap_to_zero(coefficients.b2 * in - coefficients.a2 * output));
        return output;
    }

  private:
    double s1_ = 0.0;
    double s2_ = 0.0;
};

}  // namespace pulp::signal::fdn
