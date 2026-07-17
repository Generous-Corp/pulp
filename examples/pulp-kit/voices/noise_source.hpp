#pragma once

// White-noise source for the noise-based 808 voices (handclap, maracas, and the
// snappy half of the snare).
//
// The 808 generates noise from a reverse-biased transistor junction -- a broad,
// essentially white hiss. A physical junction model would be a random process
// with the same first- and second-order statistics as a uniform white source,
// so nothing audible is lost by generating that hiss directly: the ear cannot
// distinguish two white sequences with the same spectrum. What matters for the
// clap and the shaker is the *shaping* around the noise (the flam envelope, the
// resonant band-pass, the decay), which the voices own; the source only has to
// be flat and cheap.
//
// A 64-bit xorshift* generator provides that flat spectrum with a long period
// and no allocation or state beyond a single word, so it is safe to run on the
// audio thread. The stream is deterministic for a given seed, which is what
// lets the voice tests assert on rendered statistics rather than smoke-testing.

#include <cstdint>

namespace pulp::examples {

/// Deterministic white-noise generator: xorshift64* mapped to [-1, 1).
///
/// RT contract: construction and every call are allocation-free and lock-free;
/// the entire state is one 64-bit word.
class WhiteNoise {
public:
    WhiteNoise() = default;
    explicit WhiteNoise(std::uint64_t seed) noexcept { set_seed(seed); }

    /// Reseed the stream. A zero seed would wedge xorshift at zero forever, so
    /// it is remapped to a fixed non-zero constant.
    void set_seed(std::uint64_t seed) noexcept {
        state_ = seed ? seed : 0x9E3779B97F4A7C15ull;
    }

    /// Restart the stream from the default seed so a reset voice renders the
    /// same hiss it did on the first strike -- reproducible tests, and no
    /// dependence on how many samples were drawn before a reset.
    void reset() noexcept { state_ = kDefaultSeed; }

    /// Next sample, uniform on [-1, 1).
    float process() noexcept {
        // xorshift64* -- a full-period (2^64 - 1) generator with good spectral
        // flatness for its cost.
        std::uint64_t x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        const std::uint64_t r = x * 0x2545F4914F6CDD1Dull;
        // Take the top 24 bits into [0, 1), then to [-1, 1).
        const std::uint32_t top = static_cast<std::uint32_t>(r >> 40);  // 24 bits
        return static_cast<float>(top) * (2.0f / 16777216.0f) - 1.0f;
    }

private:
    static constexpr std::uint64_t kDefaultSeed = 0x9E3779B97F4A7C15ull;
    std::uint64_t state_ = kDefaultSeed;
};

}  // namespace pulp::examples
