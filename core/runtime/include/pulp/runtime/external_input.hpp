#pragma once

/// @file external_input.hpp
/// Small, real-time-safe contract for application-owned external controls.

#include <cmath>
#include <cstdint>

namespace pulp::runtime {

/// A complete observation published by an application-owned provider.
/// Providers may add their own fields by embedding this value in a larger POD.
struct ExternalInputSample {
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_samples = 0;
    float value = 0.0f;
    float confidence = 0.0f;
    bool valid = false;
};

struct ExternalInputPolicy {
    float minimum_confidence = 0.0f;
    std::uint64_t max_age_samples = 0;
    float fallback = 0.0f;
};

/// Maps a published observation to an ephemeral control value.
/// This function performs no allocation, locking, I/O, or parameter writes.
[[nodiscard]] inline float map_external_input(const ExternalInputSample& sample,
                                              std::uint64_t now_samples,
                                              const ExternalInputPolicy& policy) noexcept {
    if (!sample.valid || !std::isfinite(sample.value) || !std::isfinite(sample.confidence) ||
        sample.confidence < policy.minimum_confidence || sample.confidence < 0.0f ||
        sample.confidence > 1.0f ||
        (policy.max_age_samples != 0 &&
         (now_samples < sample.timestamp_samples ||
          now_samples - sample.timestamp_samples > policy.max_age_samples))) {
        return policy.fallback;
    }
    return sample.value;
}

} // namespace pulp::runtime
