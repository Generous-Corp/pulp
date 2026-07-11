#pragma once

// Flow: the per-room, per-block time-varying pan generator that turns the static
// multi-room reverb into a living, moving field.
//
// At flow == 0 the rooms sit at the same constant-power positions the GPU engine
// bakes today (room k at azimuth k/(N-1)·π/2), so Flow=0 is bit-for-bit the
// current sound. Above 0 each room's azimuth drifts on its OWN rate/phase, so the
// N pan vectors are genuinely time-varying and full-rank — the irreducible case
// where the batched GPU beats any folding CPU (proven in bench.cpp
// bench_timevarying()). Constant-power is preserved at every instant
// (pan_l² + pan_r² == 1/N), so loudness never pumps as rooms move.
//
// Allocation-free: fills caller-owned buffers, safe to call per block from the
// GPU worker thread (read `flow`/`spread` from atomics on the audio side).

#include <cmath>
#include <cstdint>

namespace pulp::superconvolver {

inline float flow_hash(std::uint32_t k) {
    float x = std::sin(k * 127.1f + 0.7f) * 43758.5453f;
    return x - std::floor(x);
}

// Base (flow==0) azimuth for room k of n: evenly spread across [0, π/2].
inline float flow_base_azimuth(std::uint32_t k, std::uint32_t n) {
    constexpr float kHalfPi = 1.5707963f;
    if (n <= 1) return kHalfPi * 0.5f;
    return static_cast<float>(k) / static_cast<float>(n - 1) * kHalfPi;
}

// Fill constant-power pans for `n` rooms at time `t_seconds`.
//   flow   in [0,1] — how strongly the field moves (0 = static).
//   spread in [0,1] — how far each room wanders (depth of the drift).
inline void fill_flow_pans(std::uint32_t n, double t_seconds, float flow,
                           float spread, float* pan_l, float* pan_r) {
    constexpr float kHalfPi = 1.5707963f;
    constexpr float kTwoPi = 6.2831853f;
    const float norm = n > 0 ? 1.0f / std::sqrt(static_cast<float>(n)) : 1.0f;
    const float swing = flow * (0.4f + 0.6f * spread);  // radians at full flow

    for (std::uint32_t k = 0; k < n; ++k) {
        const float base = flow_base_azimuth(k, n);
        // Distinct rate + phase per room → the weight matrix stays full-rank.
        const float rate = 0.05f + 0.09f * flow_hash(k * 5u + 2u);   // Hz
        const float phase = flow_hash(k * 11u + 4u) * kTwoPi;
        float theta = base + swing * std::sin(kTwoPi * rate *
                                              static_cast<float>(t_seconds) + phase);
        // Reflect into [0, π/2] so pans stay in range AND constant-power holds.
        if (theta < 0.0f) theta = -theta;
        theta = std::fmod(theta, 2.0f * kHalfPi);
        if (theta > kHalfPi) theta = 2.0f * kHalfPi - theta;

        pan_l[k] = std::cos(theta) * norm;
        pan_r[k] = std::sin(theta) * norm;
    }
}

}  // namespace pulp::superconvolver
