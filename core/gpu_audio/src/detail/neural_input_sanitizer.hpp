#pragma once

#include <cmath>
#include <cstdint>

namespace pulp::gpu_audio::detail {

/// The neural node's non-finite input policy, shared by the GPU submission and
/// the CPU shadow so both see identical samples: NaN and +/-Inf become 0.0f
/// and every other sample passes through unchanged. A causal model's state
/// would otherwise carry one bad sample forever. Allocation-free and RT-safe;
/// `input` and `output` may alias. Returns the number of samples replaced.
inline std::uint32_t sanitize_nonfinite(const float* input, float* output,
                                        std::uint32_t frames) noexcept {
    std::uint32_t replaced = 0;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float x = input[i];
        const bool finite = std::isfinite(x);
        replaced += finite ? 0u : 1u;
        output[i] = finite ? x : 0.0f;
    }
    return replaced;
}

/// Number of samples sanitize_nonfinite() would replace.
inline std::uint32_t count_nonfinite(const float* input, std::uint32_t frames) noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < frames; ++i)
        count += std::isfinite(input[i]) ? 0u : 1u;
    return count;
}

} // namespace pulp::gpu_audio::detail
