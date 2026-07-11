#pragma once

// Flow: per-room, per-block time-varying constant-power pans that turn a static
// multi-room GPU reverb into a living, moving field.
//
// At depth == 0 each room sits at its base azimuth, so the pans equal the static
// constant-power layout the engine bakes at prepare() — Flow off is bit-for-bit
// the current sound. Above 0 each room's azimuth drifts on its OWN rate + phase,
// so the N pan vectors are time-varying and full-rank: the irreducible regime
// where the batched GPU beats any folding CPU. Constant-power is preserved every
// instant (pan_l² + pan_r² == norm²), so loudness never pumps as the field moves.
//
// Header-only + allocation-free: fills caller-owned buffers, safe to call per
// block from the GPU worker thread (read `depth` from an atomic on the audio side).

#include <cmath>
#include <cstdint>

namespace pulp::gpu_audio {

inline float flow_hash(std::uint32_t k) {
    float x = std::sin(k * 127.1f + 0.7f) * 43758.5453f;
    return x - std::floor(x);
}

// Base (depth==0) azimuth for room k of n: evenly spread across [0, π/2]. Matches
// GpuMultiConvolver's prepared constant-power layout (t = k/(n-1), n==1 → π/4).
inline float flow_base_azimuth(std::uint32_t k, std::uint32_t n) {
    constexpr float kHalfPi = 1.5707963f;
    if (n <= 1) return kHalfPi * 0.5f;
    return static_cast<float>(k) / static_cast<float>(n - 1) * kHalfPi;
}

// One room's constant-power pan at time `t_seconds`. `room_index` keys the
// distinct rate/phase (so the field stays full-rank); `depth`/`spread` shape the
// drift; `norm` is the per-room gain. depth==0 → exactly (cos·norm, sin·norm) of
// the base azimuth.
inline void flow_pan_room(float base_theta, std::uint32_t room_index, float depth,
                          float spread, double t_seconds, float norm,
                          float& pan_l, float& pan_r) {
    constexpr float kHalfPi = 1.5707963f;
    constexpr float kTwoPi = 6.2831853f;
    // Swing: radians of azimuth wander at full depth. ~1.2 rad (≈69°) sweeps a
    // room most of the way across the field, so the per-room pan gains move by
    // well over 6 dB — clearly audible motion, not a subtle wobble.
    const float swing = depth * (0.6f + 0.6f * spread);  // radians at full depth
    // Rate: each room drifts on its own LFO. 0.15–0.5 Hz (2–6.7 s periods) is
    // slow enough to read as an evolving field yet fast enough that the movement
    // is obvious within a couple of seconds. The previous 0.05–0.14 Hz band gave
    // 7–20 s periods — so slow that over a few seconds Flow was nearly inaudible.
    const float rate = 0.15f + 0.35f * flow_hash(room_index * 5u + 2u);   // Hz
    const float phase = flow_hash(room_index * 11u + 4u) * kTwoPi;
    float theta = base_theta + swing * std::sin(kTwoPi * rate *
                                        static_cast<float>(t_seconds) + phase);
    // Reflect into [0, π/2] so pans stay in range AND constant-power holds.
    if (theta < 0.0f) theta = -theta;
    theta = std::fmod(theta, 2.0f * kHalfPi);
    if (theta > kHalfPi) theta = 2.0f * kHalfPi - theta;
    pan_l = std::cos(theta) * norm;
    pan_r = std::sin(theta) * norm;
}

// Fill pans for `n` rooms from each room's base azimuth (the engine passes its
// prepared constant-power azimuths so depth==0 reproduces the baked layout).
inline void flow_pans_from_base(const float* base_theta, std::uint32_t n, float depth,
                                float spread, double t_seconds, float norm,
                                float* pan_l, float* pan_r) {
    for (std::uint32_t k = 0; k < n; ++k)
        flow_pan_room(base_theta[k], k, depth, spread, t_seconds, norm, pan_l[k], pan_r[k]);
}

// Convenience: even-spread base azimuths (for tests and the still renderer).
inline void fill_flow_pans(std::uint32_t n, double t_seconds, float depth,
                           float spread, float* pan_l, float* pan_r) {
    const float norm = n > 0 ? 1.0f / std::sqrt(static_cast<float>(n)) : 1.0f;
    for (std::uint32_t k = 0; k < n; ++k)
        flow_pan_room(flow_base_azimuth(k, n), k, depth, spread, t_seconds, norm,
                      pan_l[k], pan_r[k]);
}

}  // namespace pulp::gpu_audio
