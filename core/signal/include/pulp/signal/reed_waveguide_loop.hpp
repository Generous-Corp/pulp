#pragma once

/// @file reed_waveguide_loop.hpp
/// Fixed-topology whole-loop oversampled single-reed waveguide composition.

#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/waveguide_line.hpp>
#include <pulp/signal/waveguide_reed_exciter.hpp>
#include <pulp/signal/waveguide_reflection_filter.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace pulp::signal {

/// Owns and oversamples one complete reed -> bore -> bell feedback loop.
///
/// The public tuning unit is physical one-way seconds. The internal line runs
/// at `base_sample_rate * oversampling_factor`, so every oversampled callback
/// reads both boundaries, evaluates them, and advances both rails exactly once.
/// The line delay is resonator state; `latency_samples()` reports only the
/// linear-phase oversampler's base-rate algorithmic delay.
template <typename SampleType = float> class ReedWaveguideLoopT {
    static_assert(std::is_floating_point_v<SampleType>);
    using Oversampler = OversamplerT<SampleType>;

  public:
    static constexpr double default_one_way_seconds = 1.0 / (4.0 * 440.0);
    static constexpr SampleType maximum_base_rate_bell_loss_pole = SampleType{0.98};

    [[nodiscard]] bool prepare(double base_sample_rate, double max_one_way_seconds,
                               int oversampling_factor = 2) {
        if (!std::isfinite(base_sample_rate) || !(base_sample_rate > 0.0) ||
            !std::isfinite(max_one_way_seconds) ||
            (oversampling_factor != 1 && oversampling_factor != 2 &&
             oversampling_factor != 4))
            return false;
        const auto internal_rate = base_sample_rate * oversampling_factor;
        if (!std::isfinite(internal_rate) ||
            internal_rate > static_cast<double>(std::numeric_limits<SampleType>::max()) ||
            max_one_way_seconds * internal_rate <
                WaveguideLineT<SampleType>::minimum_length_samples)
            return false;

        Oversampler next_oversampler;
        if (oversampling_factor != 1) {
            next_oversampler.set_factor(oversampling_factor == 2 ? Oversampler::Factor::x2
                                                                 : Oversampler::Factor::x4);
            next_oversampler.set_sample_rate(static_cast<SampleType>(base_sample_rate));
            next_oversampler.set_quality(Oversampler::Quality::standard);
            next_oversampler.set_kind(Oversampler::Kind::linear_phase_fir);
        }

        WaveguideLineT<SampleType> next_line;
        if (!next_line.prepare(internal_rate, max_one_way_seconds))
            return false;
        const auto effective_seconds = clamped_seconds(target_one_way_seconds_, internal_rate,
                                                       max_one_way_seconds);
        next_line.set_length_samples(
            static_cast<SampleType>(effective_seconds * internal_rate));

        oversampler_ = std::move(next_oversampler);
        line_ = std::move(next_line);
        base_sample_rate_ = base_sample_rate;
        internal_sample_rate_ = internal_rate;
        max_one_way_seconds_ = max_one_way_seconds;
        target_one_way_seconds_ = effective_seconds;
        oversampling_factor_ = oversampling_factor;
        prepared_ = true;
        configure_internal_bell_pole();
        reset();
        return true;
    }

    void set_one_way_seconds(double seconds) noexcept {
        if (!std::isfinite(seconds))
            return;
        if (!prepared_) {
            target_one_way_seconds_ = std::max(0.0, seconds);
            return;
        }
        target_one_way_seconds_ =
            clamped_seconds(seconds, internal_sample_rate_, max_one_way_seconds_);
        line_.set_length_samples(
            static_cast<SampleType>(target_one_way_seconds_ * internal_sample_rate_));
    }

    [[nodiscard]] double target_one_way_seconds() const noexcept {
        return prepared_ ? static_cast<double>(line_.length_samples()) / internal_sample_rate_
                         : target_one_way_seconds_;
    }

    [[nodiscard]] double current_one_way_seconds() const noexcept {
        return prepared_
                   ? static_cast<double>(line_.current_length_samples()) / internal_sample_rate_
                   : 0.0;
    }

    [[nodiscard]] double round_trip_seconds() const noexcept {
        return prepared_ ? line_.round_trip_seconds() : 0.0;
    }

    void set_closing_pressure(SampleType value) noexcept {
        reed_.set_closing_pressure(value);
    }
    void set_flow_gain(SampleType value) noexcept { reed_.set_flow_gain(value); }
    void set_bore_impedance(SampleType value) noexcept { reed_.set_bore_impedance(value); }
    void set_bell_reflection_gain(SampleType value) noexcept {
        bell_.set_reflection_gain(value);
    }
    /// Sets the bell pole in the base-rate domain. The owned internal boundary
    /// receives `pow(pole, 1/factor)`, preserving physical decay across 1x/2x/4x.
    void set_bell_loss_pole(SampleType value) noexcept {
        if (!std::isfinite(static_cast<double>(value)))
            return;
        base_rate_bell_loss_pole_ =
            std::clamp(value, SampleType{}, maximum_base_rate_bell_loss_pole);
        configure_internal_bell_pole();
    }

    [[nodiscard]] SampleType bell_loss_pole() const noexcept {
        return base_rate_bell_loss_pole_;
    }
    [[nodiscard]] SampleType internal_bell_loss_pole() const noexcept {
        return bell_.loss_pole();
    }

    /// Returns the pressure wave arriving at the bell before reflection. This
    /// is an explicit bore pickup, not a calibrated acoustic-radiation model.
    [[nodiscard]] SampleType process(SampleType mouth_pressure) noexcept {
        if (!prepared_)
            return SampleType{};
        if (!std::isfinite(static_cast<double>(mouth_pressure)))
            mouth_pressure = last_finite_mouth_pressure_;
        else {
            mouth_pressure = std::clamp(mouth_pressure, SampleType{}, SampleType{1});
            last_finite_mouth_pressure_ = mouth_pressure;
        }

        auto whole_loop = [this](SampleType oversampled_mouth) noexcept {
            SampleType reed_arrival{};
            SampleType bell_arrival{};
            line_.read_outputs(reed_arrival, bell_arrival);
            const auto reed_reflection = reed_.process(oversampled_mouth, reed_arrival);
            const auto bell_reflection = bell_.process(bell_arrival);
            line_.push_inputs(reed_reflection, bell_reflection);
            return bell_arrival;
        };

        const auto output = oversampling_factor_ == 1
                                ? whole_loop(mouth_pressure)
                                : oversampler_.process(mouth_pressure, whole_loop);
        if (!std::isfinite(static_cast<double>(output))) {
            // The line already rejects non-finite pushes and reads. Avoid its
            // size-proportional history clear on the audio thread.
            oversampler_.reset();
            reed_.reset();
            bell_.reset();
            return SampleType{};
        }
        return output;
    }

    [[nodiscard]] int oversampling_factor() const noexcept { return oversampling_factor_; }
    [[nodiscard]] int latency_samples() const noexcept {
        return oversampling_factor_ == 1 ? 0 : oversampler_.latency_samples();
    }
    [[nodiscard]] int tail_samples() const noexcept { return -1; }
    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    void reset() noexcept {
        oversampler_.reset();
        line_.reset();
        reed_.reset();
        bell_.reset();
        last_finite_mouth_pressure_ = SampleType{};
    }

  private:
    void configure_internal_bell_pole() noexcept {
        const auto factor = std::max(1, oversampling_factor_);
        const auto mapped =
            base_rate_bell_loss_pole_ == SampleType{}
                ? SampleType{}
                : static_cast<SampleType>(std::pow(
                      static_cast<double>(base_rate_bell_loss_pole_), 1.0 / factor));
        bell_.set_loss_pole(mapped);
    }

    [[nodiscard]] static double clamped_seconds(double seconds, double internal_rate,
                                                double maximum) noexcept {
        const auto minimum = WaveguideLineT<SampleType>::minimum_length_samples / internal_rate;
        return std::clamp(seconds, minimum, maximum);
    }

    Oversampler oversampler_{};
    WaveguideLineT<SampleType> line_{};
    ReedExciterT<SampleType> reed_{};
    WaveguideReflectionFilterT<SampleType> bell_{};
    double base_sample_rate_ = 0.0;
    double internal_sample_rate_ = 0.0;
    double max_one_way_seconds_ = 0.0;
    double target_one_way_seconds_ = default_one_way_seconds;
    int oversampling_factor_ = 1;
    SampleType base_rate_bell_loss_pole_ = static_cast<SampleType>(
        WaveguideReflectionFilterT<SampleType>::default_loss_pole);
    SampleType last_finite_mouth_pressure_{};
    bool prepared_ = false;
};

using ReedWaveguideLoop = ReedWaveguideLoopT<float>;
using ReedWaveguideLoop64 = ReedWaveguideLoopT<double>;

} // namespace pulp::signal
