#include "yoga_measurement_internal.hpp"

#include <cmath>

namespace pulp::view {

float resolve_yoga_measure_dimension(float intrinsic, float available,
                                     bool available_is_defined) {
    if (intrinsic <= 0.0f) return available_is_defined ? available : 0.0f;
    return intrinsic;
}

YogaMeasurement sanitize_yoga_measurement(float width, float height) {
    const bool invalid_width = !std::isfinite(width);
    const bool invalid_height = !std::isfinite(height);
    return {invalid_width ? 0.0f : width, invalid_height ? 0.0f : height,
            invalid_width || invalid_height};
}

}  // namespace pulp::view
