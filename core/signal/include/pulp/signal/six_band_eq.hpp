#pragma once

/// @file six_band_eq.hpp
/// Fixed-role six-band parametric equalizer for real-time processing.

#include <pulp/signal/biquad.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <span>
#include <type_traits>

namespace pulp::signal {

/// A low shelf, four peaking sections, and a high shelf in series.
///
/// The template's channel count is fixed so prepare, control changes, process,
/// reset, and response inspection never allocate. Controls are plain-domain
/// values intended to be changed between blocks. The default zero-sample
/// transition preserves the legacy demo's immediate coefficient replacement.
/// An opt-in transition crossfades two complete stable cascades; recursive
/// coefficients are never interpolated. Both paths preserve filter history.
template <typename SampleType = float, std::size_t Channels = 2>
class SixBandEqT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "SixBandEqT requires a floating-point sample type");
    static_assert(Channels > 0, "SixBandEqT requires at least one channel");

public:
    static constexpr std::size_t band_count = 6;
    static constexpr std::size_t channel_count = Channels;
    static constexpr SampleType min_frequency_hz = SampleType{20};
    static constexpr SampleType max_frequency_hz = SampleType{20000};
    static constexpr SampleType min_gain_db = SampleType{-18};
    static constexpr SampleType max_gain_db = SampleType{18};
    static constexpr SampleType min_q = SampleType{0.1};
    static constexpr SampleType max_q = SampleType{12};
    static constexpr SampleType default_sample_rate = SampleType{48000};

    using Filter = BiquadT<SampleType>;
    using Type = typename Filter::Type;
    using Coefficients = BiquadCoefficientsT<SampleType>;

    struct Parameters {
        SampleType frequency_hz = SampleType{1000};
        SampleType gain_db = SampleType{0};
        SampleType q = SampleType{1};
    };

    SixBandEqT() { apply_all_parameters_immediate(); }

    /// Set the processing rate, re-clamp controls, derive coefficients, and
    /// clear all recursive history. Rates too low to represent the public
    /// 20 Hz floor, or non-finite rates, select the 48 kHz safe default.
    void prepare(SampleType sample_rate) {
        sample_rate_ = valid_sample_rate(sample_rate) ? sample_rate : default_sample_rate;
        apply_all_parameters_immediate();
        reset();
    }

    /// Set the length of future coefficient transitions. Zero is the legacy
    /// immediate mode. Already scheduled transitions retain their duration and
    /// warmed filter state; assigning the current length is an exact no-op.
    void set_transition_samples(std::size_t samples) {
        if (samples == transition_samples_) return;
        transition_samples_ = samples;
    }

    std::size_t transition_samples() const { return transition_samples_; }
    bool transition_active(std::size_t channel) const {
        return channel < Channels && transition_remaining_[channel] != 0;
    }
    bool transition_active() const {
        return std::any_of(transition_remaining_.begin(), transition_remaining_.end(),
                           [](std::size_t remaining) { return remaining != 0; });
    }

    /// Change one band's plain-domain controls without clearing filter state.
    /// Returns false for an out-of-range band and leaves the instance unchanged.
    bool set_band(std::size_t band, Parameters parameters) {
        if (band >= band_count) return false;
        parameters = sanitize(band, parameters);
        if (parameters_equal(parameters_[band], parameters)) return true;
        parameters_[band] = parameters;
        design_parameters(band);
        schedule_band(band);
        return true;
    }

    /// Atomically request all six bands. This is the preferred host-automation
    /// path when several controls change at one block boundary: every band
    /// shares one transition rather than serializing six fades.
    void set_bands(const std::array<Parameters, band_count>& parameters) {
        std::array<Parameters, band_count> sanitized{};
        bool changed = false;
        for (std::size_t band = 0; band < band_count; ++band) {
            sanitized[band] = sanitize(band, parameters[band]);
            changed = changed || !parameters_equal(parameters_[band], sanitized[band]);
        }
        if (!changed) return;
        for (std::size_t band = 0; band < band_count; ++band) {
            parameters_[band] = sanitized[band];
            design_parameters(band);
        }
        schedule_all_bands();
    }

    /// Effective controls. Invalid indices return neutral generic parameters;
    /// static role/default inspectors likewise return a peaking/neutral value.
    Parameters band(std::size_t index) const {
        return index < band_count ? parameters_[index] : Parameters{};
    }
    SampleType sample_rate() const { return sample_rate_; }

    static constexpr Type band_type(std::size_t index) {
        return index < band_count ? types_[index] : Type::peaking;
    }

    static constexpr Parameters default_band(std::size_t index) {
        return index < band_count ? defaults_[index] : Parameters{};
    }

    /// Exact normalized coefficients at the latest requested transition
    /// endpoint. During a fade both stable cascades are active; response
    /// inspection intentionally describes the destination.
    Coefficients coefficients(std::size_t band) const {
        return band < band_count ? requested_coefficients_[band] : Coefficients{};
    }

    /// Process one sample. An invalid channel fails transparent.
    SampleType process(SampleType input, std::size_t channel = 0) {
        if (channel >= Channels) return input;
        if (!transition_active(channel)) {
            for (auto& section : filters_[channel]) input = section.process(input);
            return input;
        }
        SampleType from = input;
        SampleType to = input;
        for (auto& section : filters_[channel]) from = section.process(from);
        for (auto& section : transition_filters_[channel]) to = section.process(to);
        const std::size_t completed = transition_total_[channel] - transition_remaining_[channel] + 1;
        const SampleType mix = static_cast<SampleType>(completed) /
                               static_cast<SampleType>(transition_total_[channel]);
        const SampleType output = from + (to - from) * mix;
        if (--transition_remaining_[channel] == 0) {
            filters_[channel] = transition_filters_[channel];
            if (queued_transition_[channel]) {
                const std::size_t queued_samples = queued_transition_samples_[channel];
                queued_transition_[channel] = false;
                queued_transition_samples_[channel] = 0;
                begin_transition(channel, queued_samples);
            }
        }
        return output;
    }

    /// Process one caller-owned channel in place. A null buffer with a non-zero
    /// frame count or an invalid channel is rejected without changing state.
    bool process_block(SampleType* samples, std::size_t frames, std::size_t channel = 0) {
        if (channel >= Channels || (samples == nullptr && frames != 0)) return false;
        for (std::size_t frame = 0; frame < frames; ++frame)
            samples[frame] = process(samples[frame], channel);
        return true;
    }

    /// Clear recursive history while preserving rate and effective controls.
    void reset() {
        for (auto& channel : filters_)
            for (auto& section : channel) section.reset();
        for (auto& channel : transition_filters_)
            for (auto& section : channel) section.reset();
    }

    /// Linear magnitude of the requested endpoint coefficient cascade. Query
    /// frequencies are saturated to [0, Nyquist]; non-finite queries use DC.
    double magnitude(double frequency_hz) const {
        const double omega = query_omega(frequency_hz);
        const std::complex<double> z1 = std::polar(1.0, -omega);
        const std::complex<double> z2 = z1 * z1;
        double result = 1.0;
        for (std::size_t band = 0; band < band_count; ++band) {
            const auto c = coefficients(band);
            const auto numerator = static_cast<double>(c.b0) + static_cast<double>(c.b1) * z1 +
                                   static_cast<double>(c.b2) * z2;
            const auto denominator = 1.0 + static_cast<double>(c.a1) * z1 +
                                     static_cast<double>(c.a2) * z2;
            const double denominator_magnitude = std::abs(denominator);
            if (!(denominator_magnitude > 0.0)) return 0.0;
            result *= std::abs(numerator) / denominator_magnitude;
        }
        return result;
    }

    /// Decibel magnitude of the requested endpoint cascade, floored at -200 dB.
    double magnitude_db(double frequency_hz) const {
        const double linear = magnitude(frequency_hz);
        if (!(linear > 0.0)) return -200.0;
        const double db = 20.0 * std::log10(linear);
        return std::isfinite(db) ? std::max(db, -200.0) : -200.0;
    }

    /// Fill caller-owned storage with a log-spaced response. Invalid or
    /// reversed bounds collapse to the effective lower endpoint.
    void response_curve_db(double min_hz, double max_hz, std::span<SampleType> output) const {
        if (output.empty()) return;
        const double nyquist = static_cast<double>(sample_rate_) * 0.5;
        const double lo = sanitize_query(min_hz, nyquist);
        const double hi = std::max(lo, sanitize_query(max_hz, nyquist));
        for (std::size_t i = 0; i < output.size(); ++i) {
            const double t = output.size() == 1
                                 ? 0.0
                                 : static_cast<double>(i) / static_cast<double>(output.size() - 1);
            const double frequency = lo > 0.0 && hi > 0.0
                                         ? std::exp(std::log(lo) + t * (std::log(hi) - std::log(lo)))
                                         : 0.0;
            output[i] = static_cast<SampleType>(magnitude_db(frequency));
        }
    }

private:
    static constexpr std::array<Type, band_count> types_ = {
        Type::low_shelf, Type::peaking, Type::peaking,
        Type::peaking, Type::peaking, Type::high_shelf};

    static constexpr std::array<Parameters, band_count> defaults_ = {{
        {SampleType{80}, SampleType{0}, SampleType{0.707}},
        {SampleType{250}, SampleType{0}, SampleType{1}},
        {SampleType{700}, SampleType{0}, SampleType{1.2}},
        {SampleType{2000}, SampleType{0}, SampleType{1.2}},
        {SampleType{5000}, SampleType{0}, SampleType{1}},
        {SampleType{12000}, SampleType{0}, SampleType{0.707}},
    }};

    static bool valid_sample_rate(SampleType sample_rate) {
        return std::isfinite(sample_rate) &&
               sample_rate * SampleType{0.49} >= min_frequency_hz;
    }

    Parameters sanitize(std::size_t index, Parameters parameters) const {
        const Parameters fallback = defaults_[index];
        const SampleType frequency_ceiling =
            std::min(max_frequency_hz, sample_rate_ * SampleType{0.49});
        if (!std::isfinite(parameters.frequency_hz)) parameters.frequency_hz = fallback.frequency_hz;
        if (!std::isfinite(parameters.gain_db)) parameters.gain_db = fallback.gain_db;
        if (!std::isfinite(parameters.q)) parameters.q = fallback.q;
        parameters.frequency_hz =
            std::clamp(parameters.frequency_hz, min_frequency_hz, frequency_ceiling);
        parameters.gain_db = std::clamp(parameters.gain_db, min_gain_db, max_gain_db);
        parameters.q = std::clamp(parameters.q, min_q, max_q);
        return parameters;
    }

    static bool parameters_equal(const Parameters& lhs, const Parameters& rhs) {
        return lhs.frequency_hz == rhs.frequency_hz && lhs.gain_db == rhs.gain_db &&
               lhs.q == rhs.q;
    }

    void design_parameters(std::size_t band) {
        Filter designer;
        const auto& parameters = parameters_[band];
        designer.set_coefficients(types_[band], parameters.frequency_hz, parameters.q,
                                  sample_rate_, parameters.gain_db);
        requested_coefficients_[band] = designer.coefficients();
    }

    void apply_all_parameters_immediate() {
        for (std::size_t band = 0; band < band_count; ++band) {
            parameters_[band] = sanitize(band, parameters_[band]);
            design_parameters(band);
        }
        for (std::size_t channel = 0; channel < Channels; ++channel) {
            for (std::size_t band = 0; band < band_count; ++band)
                filters_[channel][band].set_coefficients(requested_coefficients_[band]);
            transition_remaining_[channel] = 0;
            transition_total_[channel] = 0;
            queued_transition_[channel] = false;
            queued_transition_samples_[channel] = 0;
        }
    }

    void load_requested(std::array<Filter, band_count>& cascade) {
        for (std::size_t band = 0; band < band_count; ++band)
            cascade[band].set_coefficients(requested_coefficients_[band]);
    }

    void begin_transition(std::size_t channel, std::size_t samples) {
        transition_filters_[channel] = filters_[channel];
        load_requested(transition_filters_[channel]);
        transition_total_[channel] = samples;
        transition_remaining_[channel] = samples;
    }

    void queue_transition(std::size_t channel) {
        if (queued_transition_[channel]) return;
        queued_transition_[channel] = true;
        // A request made after immediate mode was selected still completes via
        // a bounded continuation: cold-retuning the audible source cascade
        // would defeat the click-safe contract of the transition in flight.
        queued_transition_samples_[channel] =
            transition_samples_ != 0 ? transition_samples_ : transition_total_[channel];
    }

    void schedule_band(std::size_t band) {
        for (std::size_t channel = 0; channel < Channels; ++channel) {
            if (!transition_active(channel)) {
                if (transition_samples_ == 0)
                    filters_[channel][band].set_coefficients(requested_coefficients_[band]);
                else
                    begin_transition(channel, transition_samples_);
            }
            else if (transition_remaining_[channel] == transition_total_[channel])
                transition_filters_[channel][band].set_coefficients(requested_coefficients_[band]);
            else
                queue_transition(channel);
        }
    }

    void schedule_all_bands() {
        for (std::size_t channel = 0; channel < Channels; ++channel) {
            if (!transition_active(channel)) {
                if (transition_samples_ == 0)
                    load_requested(filters_[channel]);
                else
                    begin_transition(channel, transition_samples_);
            }
            else if (transition_remaining_[channel] == transition_total_[channel])
                load_requested(transition_filters_[channel]);
            else
                queue_transition(channel);
        }
    }

    static double sanitize_query(double frequency_hz, double nyquist) {
        if (!std::isfinite(frequency_hz)) return 0.0;
        return std::clamp(frequency_hz, 0.0, nyquist);
    }

    double query_omega(double frequency_hz) const {
        const double nyquist = static_cast<double>(sample_rate_) * 0.5;
        const double frequency = sanitize_query(frequency_hz, nyquist);
        return std::acos(-1.0) * frequency / nyquist;
    }

    SampleType sample_rate_ = default_sample_rate;
    std::array<Parameters, band_count> parameters_ = defaults_;
    std::array<Coefficients, band_count> requested_coefficients_{};
    std::array<std::array<Filter, band_count>, Channels> filters_{};
    std::array<std::array<Filter, band_count>, Channels> transition_filters_{};
    std::array<std::size_t, Channels> transition_total_{};
    std::array<std::size_t, Channels> transition_remaining_{};
    std::array<bool, Channels> queued_transition_{};
    std::array<std::size_t, Channels> queued_transition_samples_{};
    std::size_t transition_samples_ = 0;
};

using SixBandEq = SixBandEqT<float>;
using SixBandEq64 = SixBandEqT<double>;

} // namespace pulp::signal
