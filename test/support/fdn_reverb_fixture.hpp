#pragma once

// Shared deterministic fixture for the FDN engine's two acceptance translation
// units. Keeping the neutral voicing and stimulus generator in one place is
// load-bearing: a closure test must not silently measure a different engine
// configuration from the owner suite it extends.

#include <pulp/signal/fdn_reverb.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::test::fdn_reverb {

using Engine = signal::FdnReverb;
using Param = Engine::Param;

inline constexpr double kHostRate = 48000.0;
inline constexpr double kDecayProbeHz = 1000.0;
inline constexpr int kBlock = 512;

struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};

// Deterministic white noise. No random_device anywhere in this suite: a
// stability fuzz that cannot be replayed is a rumour, not a test.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed ? seed : 1u) {}

    std::uint32_t next_u32() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    double unit() {
        return static_cast<double>(next_u32() >> 8) * (1.0 / 16777216.0);
    }

    float noise() { return static_cast<float>(unit() * 2.0 - 1.0); }

private:
    std::uint32_t state_;
};

inline Stereo render(Engine& reverb, const std::vector<float>& input,
                     int block = kBlock) {
    Stereo output;
    output.left.assign(input.size(), 0.0f);
    output.right.assign(input.size(), 0.0f);
    for (std::size_t offset = 0; offset < input.size();
         offset += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(block), input.size() - offset));
        reverb.process_block(input.data() + offset, input.data() + offset,
                             output.left.data() + offset,
                             output.right.data() + offset, n);
    }
    return output;
}

inline std::vector<float> impulse(std::size_t n, float amplitude = 1.0f) {
    std::vector<float> signal(n, 0.0f);
    signal[0] = amplitude;
    return signal;
}

inline std::vector<float> noise(std::size_t n, float amplitude,
                                std::uint32_t seed = 1234u) {
    Rng rng(seed);
    std::vector<float> signal(n);
    for (float& sample : signal) sample = amplitude * rng.noise();
    return signal;
}

// The engine at its neutral settings: no damping, modulation, shimmer, drive,
// bloom, or predelay. Everything the decay law does not describe is switched
// off so a focused measurement pins one claim.
inline void configure_neutral(Engine& reverb, double decay, int rate_index) {
    reverb.set_parameter(Param::decay, decay);
    reverb.set_parameter(Param::size, 0.5);
    reverb.set_parameter(Param::predelay, 0.0);
    reverb.set_parameter(Param::damp_hi, 0.0);
    reverb.set_parameter(Param::damp_lo, 0.0);
    reverb.set_parameter(Param::diffusion, 0.7);
    reverb.set_parameter(Param::mod, 0.0);
    reverb.set_parameter(Param::shimmer, 0.0);
    reverb.set_parameter(Param::drive, 0.0);
    reverb.set_parameter(Param::bloom, 0.0);
    reverb.set_parameter(Param::width, 1.0);
    reverb.set_parameter(Param::tank_rate, static_cast<double>(rate_index));
    reverb.snap_parameters();
    reverb.reset();
}

inline bool all_finite(const Stereo& signal) {
    for (std::size_t i = 0; i < signal.left.size(); ++i)
        if (!std::isfinite(signal.left[i]) ||
            !std::isfinite(signal.right[i]))
            return false;
    return true;
}

inline double peak(const Stereo& signal) {
    double result = 0.0;
    for (std::size_t i = 0; i < signal.left.size(); ++i) {
        result =
            std::max(result, std::abs(static_cast<double>(signal.left[i])));
        result =
            std::max(result, std::abs(static_cast<double>(signal.right[i])));
    }
    return result;
}

}  // namespace pulp::test::fdn_reverb
