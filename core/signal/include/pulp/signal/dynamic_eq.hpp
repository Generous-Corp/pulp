#pragma once

/// @file dynamic_eq.hpp
/// Fixed-state, threshold-driven single-band dynamic equalization.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/dynamics_contract.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::signal {

/// One internally keyed dynamic-EQ band.
///
/// A unity-peak band-pass section isolates the controlled band. Its output is
/// both the detector key and the parallel equalization signal:
///
///     output = input + band * (db_to_gain(dynamic_gain_db) - 1)
///
/// At the centre frequency this gives the requested gain while leaving audio
/// below threshold exactly transparent. Positive range boosts and negative
/// range attenuates. Compose several instances in series for a multiband EQ;
/// this primitive deliberately owns no crossover or spectral engine.
///
/// RT contract: fixed scalar state only. After `prepare()`, `process()`, block
/// processing, `reset()`, and accessors allocate no memory, lock nothing, and
/// perform no I/O. Parameters are control-side/block-boundary operations.
template <typename SampleType = float> class DynamicEqBandT {
  public:
    static constexpr SampleType min_frequency_hz = SampleType{20};
    static constexpr SampleType max_sample_rate = SampleType{768000};
    static constexpr SampleType min_q = SampleType{0.1};
    static constexpr SampleType max_q = SampleType{12};
    static constexpr SampleType min_threshold_db = SampleType{-120};
    static constexpr SampleType max_threshold_db = SampleType{0};
    static constexpr SampleType min_range_db = SampleType{-24};
    static constexpr SampleType max_range_db = SampleType{24};

    struct Parameters {
        SampleType frequency_hz = SampleType{1000};
        SampleType q = SampleType{1};
        SampleType threshold_db = SampleType{-24};
        SampleType range_db = SampleType{-6};
        SampleType transition_db = SampleType{6};
        SampleType attack_ms = SampleType{5};
        SampleType release_ms = SampleType{80};
    };

    DynamicEqBandT() {
        redesign_();
    }

    void prepare(SampleType sample_rate) {
        sample_rate_ = valid_sample_rate_(sample_rate) ? sample_rate : SampleType{48000};
        parameters_ = clamp_(parameters_);
        detector_.prepare(sample_rate_);
        detector_.set_attack_ms(parameters_.attack_ms);
        detector_.set_release_ms(parameters_.release_ms);
        redesign_();
        reset();
    }

    /// Apply one coherent parameter set. Non-finite fields reject the whole
    /// update; finite out-of-range values are clamped to the public bounds.
    bool set_parameters(Parameters requested) {
        if (!finite_(requested))
            return false;
        requested = clamp_(requested);
        const bool filter_changed =
            requested.frequency_hz != parameters_.frequency_hz || requested.q != parameters_.q;
        parameters_ = requested;
        detector_.set_attack_ms(parameters_.attack_ms);
        detector_.set_release_ms(parameters_.release_ms);
        if (filter_changed)
            redesign_();
        return true;
    }

    Parameters parameters() const noexcept {
        return parameters_;
    }

    SampleType process(SampleType input) noexcept {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }

        const SampleType band = band_filter_.process(input);
        if (!std::isfinite(static_cast<double>(band))) {
            reset();
            return SampleType{0};
        }

        detector_level_ = detector_.process(band);
        const SampleType level_db = detector_.current_db(SampleType{-160});
        const SampleType over_db = level_db - parameters_.threshold_db;
        activity_ = std::clamp(over_db / parameters_.transition_db, SampleType{0}, SampleType{1});
        dynamic_gain_db_ = parameters_.range_db * activity_;

        if (parameters_.range_db == SampleType{0})
            return input;
        const SampleType band_gain = std::pow(SampleType{10}, dynamic_gain_db_ / SampleType{20});
        const SampleType output = input + band * (band_gain - SampleType{1});
        if (!std::isfinite(static_cast<double>(output))) {
            reset();
            return SampleType{0};
        }
        return output;
    }

    bool process_block(SampleType* samples, std::size_t frames) noexcept {
        if (samples == nullptr && frames != 0)
            return false;
        for (std::size_t i = 0; i < frames; ++i)
            samples[i] = process(samples[i]);
        return true;
    }

    void reset() noexcept {
        band_filter_.reset();
        detector_.reset();
        detector_level_ = SampleType{0};
        activity_ = SampleType{0};
        dynamic_gain_db_ = SampleType{0};
    }

    SampleType detector_level() const noexcept {
        return detector_level_;
    }
    SampleType activity() const noexcept {
        return activity_;
    }
    SampleType dynamic_gain_db() const noexcept {
        return dynamic_gain_db_;
    }
    GainReduction gain_reduction() const noexcept {
        return GainReduction::from_signed_db(static_cast<double>(dynamic_gain_db_));
    }
    BiquadCoefficientsT<SampleType> band_coefficients() const noexcept {
        return band_filter_.coefficients();
    }
    SampleType sample_rate() const noexcept {
        return sample_rate_;
    }
    static constexpr int latency_samples() noexcept {
        return 0;
    }

  private:
    static bool valid_sample_rate_(SampleType sample_rate) noexcept {
        return std::isfinite(static_cast<double>(sample_rate)) &&
               sample_rate > min_frequency_hz * SampleType{2.1} && sample_rate <= max_sample_rate;
    }

    static bool finite_(const Parameters& p) noexcept {
        return std::isfinite(static_cast<double>(p.frequency_hz)) &&
               std::isfinite(static_cast<double>(p.q)) &&
               std::isfinite(static_cast<double>(p.threshold_db)) &&
               std::isfinite(static_cast<double>(p.range_db)) &&
               std::isfinite(static_cast<double>(p.transition_db)) &&
               std::isfinite(static_cast<double>(p.attack_ms)) &&
               std::isfinite(static_cast<double>(p.release_ms));
    }

    Parameters clamp_(Parameters p) const noexcept {
        const SampleType max_frequency =
            std::max(min_frequency_hz, sample_rate_ * SampleType{0.45});
        p.frequency_hz = std::clamp(p.frequency_hz, min_frequency_hz, max_frequency);
        p.q = std::clamp(p.q, min_q, max_q);
        p.threshold_db = std::clamp(p.threshold_db, min_threshold_db, max_threshold_db);
        p.range_db = std::clamp(p.range_db, min_range_db, max_range_db);
        p.transition_db = std::clamp(p.transition_db, SampleType{0.1}, SampleType{60});
        p.attack_ms = std::clamp(p.attack_ms, SampleType{0.01}, SampleType{2000});
        p.release_ms = std::clamp(p.release_ms, SampleType{0.01}, SampleType{2000});
        return p;
    }

    void redesign_() {
        band_filter_.set_coefficients(BiquadT<SampleType>::Type::bandpass, parameters_.frequency_hz,
                                      parameters_.q, sample_rate_);
    }

    BiquadT<SampleType> band_filter_{};
    EnvelopeFollowerT<SampleType> detector_{};
    Parameters parameters_{};
    SampleType sample_rate_ = SampleType{48000};
    SampleType detector_level_ = SampleType{0};
    SampleType activity_ = SampleType{0};
    SampleType dynamic_gain_db_ = SampleType{0};
};

using DynamicEqBand = DynamicEqBandT<float>;
using DynamicEqBand64 = DynamicEqBandT<double>;

} // namespace pulp::signal
