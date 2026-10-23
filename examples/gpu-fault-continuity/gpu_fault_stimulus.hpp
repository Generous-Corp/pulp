#pragma once

// The plug-in's own deterministic stimulus, for runs where the host's input
// must not be used or stored (WP-K live-mode rows, where the input is a real
// device). Sample n is a pure function of n, so the plug-in can key it to the
// project position and the harness can rebuild any span of it:
// tools/testing/daw-smoke/gpu_fault_continuity.py (internal_stimulus) mirrors it exactly.
//
//   0.15 * sin(2*pi * ((220 * n) mod rate) / rate)
//   + (0.125 if (n / 2400) is even else 0.025) * u(n)
//
// where u(n) is splitmix64(n) mapped to [-1, 1). Peaks stay near 0.3, so the
// convolved output stays well below full scale.

#include <cmath>
#include <cstdint>

namespace pulp::examples::gpu_fault {

inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline float internal_stimulus(std::uint64_t n, std::uint32_t rate) noexcept {
    // The top 53 bits, so the uniform is exact in a double on both sides.
    const double u = static_cast<double>(splitmix64(n) >> 11) * (1.0 / 9007199254740992.0);
    const double noise = (n / 2400) % 2 == 0 ? 0.125 : 0.025;
    const double phase = static_cast<double>((220ull * n) % rate) / static_cast<double>(rate);
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    return static_cast<float>(0.15 * std::sin(kTwoPi * phase) + noise * (2.0 * u - 1.0));
}

} // namespace pulp::examples::gpu_fault
