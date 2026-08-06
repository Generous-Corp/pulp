#pragma once

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/frequency_response.hpp>
#include <pulp/signal/noise_source.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pulp::signal {

/// Continuous spectral-tilt noise source built from NoiseSourceT's canonical
/// seeded white stream and a fixed cascade of canonical BiquadT shelves.
///
/// `tilt_db_per_octave` is a power-spectral-density slope in dB/octave over the
/// audio design band (31.25 Hz to min(16 kHz, 0.4 * sample_rate)). Its legal
/// range is [-6, +6] dB/octave; zero is bit-identical to NoiseSourceT::white().
/// Gain is normalized to unity at the 1 kHz reference, and `level` is a
/// dimensionless multiplier on [0, 1]. The filtered distribution remains
/// zero-mean but is no longer uniform except at zero tilt.
///
/// `prepare()` and `set_tilt_db_per_octave()` are bounded control-side calls
/// that design coefficients and clear filter history. `process()`, `reset()`,
/// seed access, and the block overload are allocation-free and lock-free.
/// BiquadT snaps recursive denormals; a non-finite output clears all filter
/// state and emits zero while incrementing `fault_count()`.
///
/// The public seed selects the complete initial random condition. Resetting
/// rewinds both that stream and all filters, so output is invariant to block
/// partition. Latency and tail are zero samples.
template <typename SampleType = float> class NoiseTiltT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    static constexpr double kMinSampleRate = 8000.0;
    static constexpr double kMaxSampleRate = 384000.0;
    static constexpr double kMinTiltDbPerOctave = -6.0;
    static constexpr double kMaxTiltDbPerOctave = 6.0;
    static constexpr double kReferenceHz = 1000.0;
    static constexpr double kBandLowHz = 31.25;
    static constexpr double kBandHighHz = 16000.0;
    static constexpr std::size_t kShelfCount = 9;
    static constexpr std::uint32_t kDefaultSeed = NoiseSourceT<SampleType>::default_seed;

    bool prepare(double sample_rate) {
        if (!(std::isfinite(sample_rate) && sample_rate >= kMinSampleRate &&
              sample_rate <= kMaxSampleRate))
            return false;
        sample_rate_ = sample_rate;
        white_.prepare(sample_rate_);
        design_filters();
        reset();
        return true;
    }

    bool set_tilt_db_per_octave(double tilt) {
        if (!(std::isfinite(tilt) && tilt >= kMinTiltDbPerOctave && tilt <= kMaxTiltDbPerOctave))
            return false;
        tilt_db_per_octave_ = tilt;
        design_filters();
        reset_filters();
        return true;
    }

    bool set_level(SampleType level) noexcept {
        if (!(std::isfinite(static_cast<double>(level)) && level >= SampleType{} &&
              level <= SampleType{1}))
            return false;
        level_ = level;
        return true;
    }

    void set_seed(std::uint32_t seed) noexcept {
        white_.set_seed(seed);
        reset_filters();
        fault_count_ = 0;
    }

    void reset() noexcept {
        white_.reset();
        reset_filters();
        fault_count_ = 0;
    }

    SampleType process() noexcept {
        SampleType output = white_.white();
        if (tilt_db_per_octave_ != 0.0) {
            for (auto& shelf : shelves_)
                output = shelf.process(output);
            output *= static_cast<SampleType>(reference_gain_);
        }
        output *= level_;
        output = snap_to_zero(output);
        if (std::isfinite(static_cast<double>(output)))
            return output;
        ++fault_count_;
        reset_filters();
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
    double tilt_db_per_octave() const noexcept {
        return tilt_db_per_octave_;
    }
    SampleType level() const noexcept {
        return level_;
    }
    std::uint32_t seed() const noexcept {
        return white_.seed();
    }
    std::uint64_t fault_count() const noexcept {
        return fault_count_;
    }

    static constexpr int latency_samples() noexcept {
        return 0;
    }
    static constexpr int tail_samples() noexcept {
        return 0;
    }

  private:
    void design_filters() {
        const double high_hz = std::min(kBandHighHz, 0.4 * sample_rate_);
        const double octaves = std::log2(high_hz / kBandLowHz);
        const double stage_gain_db =
            tilt_db_per_octave_ * octaves / static_cast<double>(kShelfCount);
        const double ratio = high_hz / kBandLowHz;
        for (std::size_t index = 0; index < shelves_.size(); ++index) {
            const double position =
                (static_cast<double>(index) + 0.5) / static_cast<double>(kShelfCount);
            const double center_hz = kBandLowHz * std::pow(ratio, position);
            shelves_[index].set_coefficients(
                BiquadT<SampleType>::Type::high_shelf, static_cast<SampleType>(center_hz),
                static_cast<SampleType>(0.7071067811865476), static_cast<SampleType>(sample_rate_),
                static_cast<SampleType>(stage_gain_db));
        }

        double reference_db = 0.0;
        for (const auto& shelf : shelves_)
            reference_db += magnitude_db(shelf, kReferenceHz, sample_rate_);
        reference_gain_ = std::pow(10.0, -reference_db / 20.0);
    }

    void reset_filters() noexcept {
        for (auto& shelf : shelves_)
            shelf.reset();
    }

    NoiseSourceT<SampleType> white_{};
    std::array<BiquadT<SampleType>, kShelfCount> shelves_{};
    double sample_rate_ = 44100.0;
    double tilt_db_per_octave_ = 0.0;
    double reference_gain_ = 1.0;
    SampleType level_ = SampleType{1};
    std::uint64_t fault_count_ = 0;
};

using NoiseTilt = NoiseTiltT<float>;
using NoiseTilt64 = NoiseTiltT<double>;

} // namespace pulp::signal
