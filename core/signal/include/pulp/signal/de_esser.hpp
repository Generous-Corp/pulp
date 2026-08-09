#pragma once

/// @file de_esser.hpp
/// Split-band de-essing with a frequency-selective detector.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/dynamics_contract.hpp>
#include <pulp/signal/linkwitz_riley.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pulp::signal {

/// A bounded mono de-esser primitive.
///
/// A band-pass detector measures sibilance independently from the audio split.
/// Above threshold, only the LR4 high band is attenuated, while the low band is
/// retained and recombined. Use one instance per channel; linked-stereo policy
/// belongs to the caller because it requires an explicit channel-link contract.
///
/// RT contract: after `prepare()`, processing, bypass/listen changes, metering,
/// and `reset()` are fixed-state and allocate no memory. `prepare()` and
/// `set_params()` redesign coefficients and reset signal history, so call them
/// at a control-rate boundary rather than inside the sample loop.
template <typename SampleType = float> class DeEsserT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    enum class OutputMode {
        normal,
        detector_listen,
    };

    struct Params {
        SampleType threshold_db = SampleType{-24};
        SampleType range_db = SampleType{12};
        SampleType attack_ms = SampleType{1};
        SampleType release_ms = SampleType{80};
        SampleType detector_frequency_hz = SampleType{6500};
        SampleType detector_q = SampleType{1.5};
        SampleType split_frequency_hz = SampleType{4500};
    };

    /// Establish sample rate and coefficients. Invalid configurations are
    /// rejected without partially preparing the processor.
    bool prepare(SampleType sample_rate, const Params& params = {}) noexcept {
        if (!valid_configuration(sample_rate, params))
            return false;

        sample_rate_ = sample_rate;
        params_ = params;
        configure_filters();
        prepared_ = true;
        healthy_ = true;
        reset();
        return true;
    }

    /// Atomically adopt a new valid configuration. The previous configuration
    /// and state remain in force when validation fails.
    bool set_params(const Params& params) noexcept {
        if (!prepared_ || !valid_configuration(sample_rate_, params))
            return false;
        params_ = params;
        configure_filters();
        reset();
        return true;
    }

    const Params& params() const noexcept {
        return params_;
    }
    SampleType sample_rate() const noexcept {
        return prepared_ ? sample_rate_ : SampleType{0};
    }
    bool prepared() const noexcept {
        return prepared_;
    }
    bool healthy() const noexcept {
        return healthy_;
    }
    std::uint64_t fault_count() const noexcept {
        return fault_count_;
    }

    /// Bypass is sample-exact while the detector and split continue tracking,
    /// so leaving bypass does not resume from stale state.
    void set_bypassed(bool bypassed) noexcept {
        bypassed_ = bypassed;
    }
    bool bypassed() const noexcept {
        return bypassed_;
    }

    void set_output_mode(OutputMode mode) noexcept {
        output_mode_ = mode;
    }
    OutputMode output_mode() const noexcept {
        return output_mode_;
    }

    SampleType process(SampleType input) noexcept {
        if (!prepared_ || !std::isfinite(input))
            return recover(input);

        const SampleType detector_band = detector_filter_.process(input);
        const SampleType detector_level = detector_envelope_.process(detector_band);
        if (!std::isfinite(detector_band) || !std::isfinite(detector_level))
            return recover(input);

        detector_level_db_ = linear_to_db(detector_level);
        reduction_db_ =
            std::clamp(detector_level_db_ - params_.threshold_db, SampleType{0}, params_.range_db);

        const auto split = crossover_.process(input);
        const SampleType gain = std::pow(SampleType{10}, -reduction_db_ / SampleType{20});
        const SampleType processed = split.low + split.high * gain;
        if (!std::isfinite(processed))
            return recover(input);

        healthy_ = true;
        if (bypassed_)
            return input;
        if (output_mode_ == OutputMode::detector_listen)
            return detector_band;
        return processed;
    }

    void process(const SampleType* input, SampleType* output, std::size_t num_samples) noexcept {
        if (input == nullptr || output == nullptr)
            return;
        for (std::size_t i = 0; i < num_samples; ++i)
            output[i] = process(input[i]);
    }

    void process(SampleType* buffer, std::size_t num_samples) noexcept {
        process(buffer, buffer, num_samples);
    }

    /// Positive attenuation magnitude, suitable for direct display.
    SampleType gain_reduction_db() const noexcept {
        return reduction_db_;
    }
    GainReduction gain_reduction() const noexcept {
        return GainReduction::from_magnitude_db(static_cast<double>(reduction_db_));
    }
    SampleType detector_level_db() const noexcept {
        return detector_level_db_;
    }

    /// Clear signal history while retaining configuration, bypass, and output
    /// mode. The next finite sample starts from zero detector history.
    void reset() noexcept {
        detector_filter_.reset();
        detector_envelope_.reset();
        crossover_.reset();
        detector_level_db_ = minimum_detector_db;
        reduction_db_ = SampleType{0};
        healthy_ = prepared_;
    }

  private:
    static constexpr SampleType minimum_detector_db = SampleType{-160};

    static bool valid_configuration(SampleType sample_rate, const Params& params) noexcept {
        if (!(std::isfinite(sample_rate) && sample_rate >= SampleType{1000}))
            return false;
        const SampleType nyquist = sample_rate * SampleType{0.5};
        return std::isfinite(params.threshold_db) && params.threshold_db >= SampleType{-120} &&
               params.threshold_db <= SampleType{24} && std::isfinite(params.range_db) &&
               params.range_db >= SampleType{0} && params.range_db <= SampleType{60} &&
               std::isfinite(params.attack_ms) && params.attack_ms >= SampleType{0.01} &&
               params.attack_ms <= SampleType{1000} && std::isfinite(params.release_ms) &&
               params.release_ms >= SampleType{0.01} && params.release_ms <= SampleType{5000} &&
               std::isfinite(params.detector_frequency_hz) &&
               params.detector_frequency_hz >= SampleType{20} &&
               params.detector_frequency_hz < nyquist * SampleType{0.98} &&
               std::isfinite(params.detector_q) && params.detector_q >= SampleType{0.1} &&
               params.detector_q <= SampleType{20} && std::isfinite(params.split_frequency_hz) &&
               params.split_frequency_hz >= SampleType{20} &&
               params.split_frequency_hz < nyquist * SampleType{0.98};
    }

    static SampleType linear_to_db(SampleType value) noexcept {
        const SampleType bounded = std::max(value, std::numeric_limits<SampleType>::min());
        const SampleType db = SampleType{20} * std::log10(bounded);
        return std::max(minimum_detector_db, db);
    }

    void configure_filters() noexcept {
        detector_filter_.set_coefficients(BiquadT<SampleType>::Type::bandpass,
                                          params_.detector_frequency_hz, params_.detector_q,
                                          sample_rate_);
        detector_envelope_.prepare(sample_rate_);
        detector_envelope_.set_mode(EnvelopeFollowerT<SampleType>::Mode::peak);
        detector_envelope_.set_attack_ms(params_.attack_ms);
        detector_envelope_.set_release_ms(params_.release_ms);
        crossover_.set_frequency_precise(params_.split_frequency_hz, sample_rate_);
    }

    SampleType recover(SampleType input) noexcept {
        if (prepared_)
            reset();
        healthy_ = false;
        ++fault_count_;
        return std::isfinite(input) ? input : SampleType{0};
    }

    Params params_{};
    SampleType sample_rate_ = SampleType{0};
    BiquadT<SampleType> detector_filter_{};
    EnvelopeFollowerT<SampleType> detector_envelope_{};
    LinkwitzRileyT<SampleType> crossover_{};
    SampleType detector_level_db_ = minimum_detector_db;
    SampleType reduction_db_ = SampleType{0};
    OutputMode output_mode_ = OutputMode::normal;
    bool prepared_ = false;
    bool healthy_ = false;
    bool bypassed_ = false;
    std::uint64_t fault_count_ = 0;
};

using DeEsser = DeEsserT<float>;
using DeEsser64 = DeEsserT<double>;

} // namespace pulp::signal
