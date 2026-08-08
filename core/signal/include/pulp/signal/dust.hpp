#pragma once

#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pulp::signal {

/// Amplitude law applied when a DustT event occurs.
enum class DustAmplitudeDistribution : std::uint8_t {
    constant,         ///< Exactly `level`.
    uniform_unipolar, ///< Uniform on [0, level).
    uniform_bipolar,  ///< Uniform on [-level, level).
};

/// Deterministic discrete-time Poisson impulse source.
///
/// Each sample is an independent Bernoulli trial with probability
/// `density_hz / sample_rate`. This is the sample-grid form of a homogeneous
/// Poisson process: its expected event rate is exactly `density_hz`, intervals
/// are geometrically distributed, and at most one event can occupy a sample.
/// Density is therefore legal on [0, sample_rate] Hz. Output level is a
/// dimensionless magnitude on [0, 1]; bipolar amplitudes occupy [-level,
/// level), while the other laws occupy [0, level]. Event timing and amplitude
/// use independent streams derived from the same public seed, so changing the
/// amplitude law or level never moves an event.
///
/// The public seed is the complete initial random condition. `set_seed()`
/// selects it and `reset()` rewinds to it; no clock or device entropy is read.
/// Identical parameter histories therefore produce identical samples under
/// every block partition.
///
/// RT contract: `process()` and its block overload are bounded, allocation-free
/// scalar work. There is no recursive floating-point state, so denormals cannot
/// accumulate. Every generated value is finite. Latency and tail are zero.
template <typename SampleType = float> class DustT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    static constexpr double kDefaultSampleRate = 48000.0;
    static constexpr std::uint32_t kDefaultSeed = 0xD0571234u;

    DustT() {
        reset();
    }

    bool prepare(double sample_rate) noexcept {
        if (!(std::isfinite(sample_rate) && sample_rate > 0.0) || density_hz_ > sample_rate)
            return false;
        sample_rate_ = sample_rate;
        probability_ = density_hz_ / sample_rate_;
        return true;
    }

    bool set_density_hz(double density_hz) noexcept {
        if (!(std::isfinite(density_hz) && density_hz >= 0.0 && density_hz <= sample_rate_))
            return false;
        density_hz_ = density_hz;
        probability_ = density_hz_ / sample_rate_;
        return true;
    }

    bool set_level(SampleType level) noexcept {
        if (!(std::isfinite(static_cast<double>(level)) && level >= SampleType{} &&
              level <= SampleType{1}))
            return false;
        level_ = level;
        return true;
    }

    bool set_distribution(DustAmplitudeDistribution distribution) noexcept {
        switch (distribution) {
        case DustAmplitudeDistribution::constant:
        case DustAmplitudeDistribution::uniform_unipolar:
        case DustAmplitudeDistribution::uniform_bipolar:
            distribution_ = distribution;
            return true;
        }
        return false;
    }

    void set_seed(std::uint32_t seed) noexcept {
        seed_ = seed == 0u ? Xorshift32::kDefaultSeed : seed;
        reset();
    }

    void reset() noexcept {
        event_rng_.seed(seed_);
        amplitude_rng_.seed(amplitude_seed(seed_));
        triggered_ = false;
    }

    SampleType process() noexcept {
        triggered_ = event_rng_.next_unit<double>() < probability_;
        if (!triggered_)
            return SampleType{};
        switch (distribution_) {
        case DustAmplitudeDistribution::constant:
            return level_;
        case DustAmplitudeDistribution::uniform_unipolar:
            return level_ * amplitude_rng_.next_unit<SampleType>();
        case DustAmplitudeDistribution::uniform_bipolar:
            return level_ * amplitude_rng_.next_bipolar<SampleType>();
        }
        return SampleType{};
    }

    void process(SampleType* output, std::size_t frames) noexcept {
        if (output == nullptr)
            return;
        for (std::size_t frame = 0; frame < frames; ++frame)
            output[frame] = process();
    }

    double sample_rate() const noexcept {
        return sample_rate_;
    }
    double density_hz() const noexcept {
        return density_hz_;
    }
    SampleType level() const noexcept {
        return level_;
    }
    DustAmplitudeDistribution distribution() const noexcept {
        return distribution_;
    }
    std::uint32_t seed() const noexcept {
        return seed_;
    }
    bool triggered() const noexcept {
        return triggered_;
    }

    static constexpr int latency_samples() noexcept {
        return 0;
    }
    static constexpr int tail_samples() noexcept {
        return 0;
    }

  private:
    static std::uint32_t amplitude_seed(std::uint32_t seed) noexcept {
        const auto derived = static_cast<std::uint32_t>(mix64(seed, 0u, 0x44555354u));
        return derived == 0u ? Xorshift32::kDefaultSeed : derived;
    }

    Xorshift32 event_rng_{kDefaultSeed};
    Xorshift32 amplitude_rng_{amplitude_seed(kDefaultSeed)};
    double sample_rate_ = kDefaultSampleRate;
    double density_hz_ = 0.0;
    double probability_ = 0.0;
    SampleType level_ = SampleType{1};
    DustAmplitudeDistribution distribution_ = DustAmplitudeDistribution::constant;
    std::uint32_t seed_ = kDefaultSeed;
    bool triggered_ = false;
};

using Dust = DustT<float>;
using Dust64 = DustT<double>;

} // namespace pulp::signal
