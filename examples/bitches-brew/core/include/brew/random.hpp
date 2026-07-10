#pragma once

// Randomness that survives a bounce.
//
// Every other CV utility generates random values from a running generator: a new
// level each LFO cycle, each step, each note. That cannot be reproduced. Render
// the project twice and the modulation lands differently, which for a signal
// patched into a filter cutoff is the difference between a take and a rumour.
//
// So there is no generator here. There is a hash. The "random" value at cycle 91
// is a pure function of 91 and a seed, exactly as the LFO's phase at bar 57 is a
// pure function of bar 57. The sequence is unpredictable, unbounded, and
// non-repeating along the timeline, and it is identical on every render, on every
// machine, forever. Reroll it by changing the seed, not by pressing play again.

#include <cstdint>

namespace pulp::examples::brew {

/// SplitMix64's finalizer. A mixing function, not a generator: it holds no state
/// to advance, so the same input yields the same output everywhere.
[[nodiscard]] inline constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/// A unit value in `[0, 1)`, keyed on a signed index and a seed. The coin a
/// probabilistic decision is tossed with.
[[nodiscard]] inline float hash_unit(std::int64_t index, std::uint32_t seed) noexcept {
    const std::uint64_t h =
        mix64(static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL ^ seed);
    // Top 24 bits, so the conversion to float is exact.
    return static_cast<float>(static_cast<std::uint32_t>(h >> 40)) / 16777216.0f;
}

/// A bipolar value in `[-1, +1)`, keyed on a signed index and a seed.
///
/// Multiplying the index by an odd constant before mixing keeps adjacent indices
/// far apart in the hash's input, so consecutive steps do not correlate.
[[nodiscard]] inline float hash_bipolar(std::int64_t index,
                                        std::uint32_t seed) noexcept {
    const std::uint64_t h =
        mix64(static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL ^ seed);
    // Top 24 bits: more resolution than a control voltage can carry, and the
    // conversion to float is exact.
    const auto bits = static_cast<std::uint32_t>(h >> 40);
    const float unit = static_cast<float>(bits) / 8388608.0f;  // [0, 2)
    return unit - 1.0f;                                        // [-1, 1)
}

}  // namespace pulp::examples::brew
