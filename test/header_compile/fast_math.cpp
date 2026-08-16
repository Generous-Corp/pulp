#include <pulp/signal/fast_math.hpp>

#include <cmath>

void use_fast_math_header() {
    const float value = pulp::signal::FastMath::exp2(0.5f);
    (void)std::isfinite(value);
}
