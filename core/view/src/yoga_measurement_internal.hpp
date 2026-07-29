#pragma once

namespace pulp::view {

struct YogaMeasurement {
    float width;
    float height;
    bool rejected_non_finite;
};

// Resolves a missing (zero/negative) intrinsic dimension against Yoga's
// constraint. Yoga encodes an undefined available dimension as NaN, so it must
// never be echoed back to Yoga as the measured result.
float resolve_yoga_measure_dimension(float intrinsic, float available,
                                     bool available_is_defined);

// Enforces the finite-dimension contract at the View/Yoga boundary. The caller
// owns contextual diagnostics because it still has the originating View.
YogaMeasurement sanitize_yoga_measurement(float width, float height);

}  // namespace pulp::view
