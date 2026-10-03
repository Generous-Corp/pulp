#pragma once

/// @file parametric_eq.hpp
/// Fixed-capacity, arbitrary-band parametric equalizer.

#include <pulp/signal/frequency_response.hpp>
#include <pulp/signal/sos_cascade.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class ParametricEqBandType : std::uint8_t { low_shelf, peaking, high_shelf };

template <typename SampleType> struct ParametricEqBandT {
    ParametricEqBandType type = ParametricEqBandType::peaking;
    SampleType frequency_hz = SampleType{1000};
    SampleType gain_db = SampleType{0};
    SampleType q = SampleType{1};
    bool enabled = true;
    bool solo = false;

    friend constexpr bool operator==(const ParametricEqBandT&, const ParametricEqBandT&) = default;
};

using ParametricEqBand = ParametricEqBandT<float>;

enum class ParametricEqPrepareStatus { prepared, invalid_sample_rate, invalid_capacity };

enum class ParametricEqConfigureStatus {
    configured,
    not_prepared,
    over_capacity,
    invalid_transition,
    non_finite,
    frequency_out_of_range,
    frequencies_not_strictly_increasing,
    gain_out_of_range,
    q_out_of_range,
    invalid_solo,
    unstable,
    transition_in_progress,
};

/// A bounded series cascade of arbitrary parametric bands.
///
/// Configuration is transactional and control-rate: all bands are validated and
/// designed before the live cascade changes. Processing is fixed-storage and
/// allocation-free. A non-zero transition crossfades complete SOS cascades;
/// coefficients are never interpolated. If one or more bands is soloed, only
/// enabled solo bands are admitted. With no solo bands, every enabled band runs.
template <typename SampleType = float, std::size_t MaxBands = 31> class ParametricEqT {
    static_assert(std::is_floating_point_v<SampleType>, "ParametricEqT requires floating point");
    static_assert(MaxBands > 0, "ParametricEqT requires non-zero band storage");

  public:
    using Sample = SampleType;
    using Band = ParametricEqBandT<SampleType>;
    using Coefficients = BiquadCoefficientsT<SampleType>;

    static constexpr SampleType minimum_sample_rate_hz = SampleType{8000};
    static constexpr SampleType maximum_sample_rate_hz = SampleType{384000};
    static constexpr SampleType minimum_frequency_hz = SampleType{20};
    static constexpr SampleType maximum_frequency_hz = SampleType{20000};
    static constexpr SampleType minimum_gain_db = SampleType{-24};
    static constexpr SampleType maximum_gain_db = SampleType{24};
    static constexpr SampleType minimum_q = SampleType{0.1};
    static constexpr SampleType maximum_q = SampleType{12};

    ParametricEqPrepareStatus prepare(SampleType sample_rate,
                                      std::size_t band_capacity = MaxBands) noexcept {
        if (!(std::isfinite(sample_rate) && sample_rate >= minimum_sample_rate_hz &&
              sample_rate <= maximum_sample_rate_hz))
            return ParametricEqPrepareStatus::invalid_sample_rate;
        if (band_capacity == 0 || band_capacity > MaxBands)
            return ParametricEqPrepareStatus::invalid_capacity;
        (void)cascades_[0].prepare(band_capacity);
        (void)cascades_[1].prepare(band_capacity);
        sample_rate_ = sample_rate;
        capacity_ = band_capacity;
        active_index_ = 0;
        active_count_ = requested_count_ = 0;
        transition_total_ = transition_remaining_ = 0;
        requested_bands_ = {};
        active_bands_ = {};
        requested_coefficients_ = {};
        return ParametricEqPrepareStatus::prepared;
    }

    ParametricEqConfigureStatus configure(std::span<const Band> bands,
                                          std::size_t transition_samples = 0) noexcept {
        if (!prepared()) return ParametricEqConfigureStatus::not_prepared;
        if (transitioning()) return ParametricEqConfigureStatus::transition_in_progress;
        if (bands.size() > capacity_) return ParametricEqConfigureStatus::over_capacity;
        if (transition_samples == std::numeric_limits<std::size_t>::max())
            return ParametricEqConfigureStatus::invalid_transition;

        std::array<Coefficients, MaxBands> candidate{};
        std::array<Band, MaxBands> selected{};
        std::size_t selected_count = 0;
        bool has_solo = false;
        SampleType previous_frequency = SampleType{0};
        const SampleType ceiling = supported_frequency_ceiling_hz();
        for (std::size_t i = 0; i < bands.size(); ++i) {
            const Band band = bands[i];
            if (band.solo && !band.enabled) return ParametricEqConfigureStatus::invalid_solo;
            if (!(std::isfinite(band.frequency_hz) && std::isfinite(band.gain_db) &&
                  std::isfinite(band.q)))
                return ParametricEqConfigureStatus::non_finite;
            if (band.frequency_hz < minimum_frequency_hz || band.frequency_hz > ceiling)
                return ParametricEqConfigureStatus::frequency_out_of_range;
            if (i != 0 && !(band.frequency_hz > previous_frequency))
                return ParametricEqConfigureStatus::frequencies_not_strictly_increasing;
            if (band.gain_db < minimum_gain_db || band.gain_db > maximum_gain_db)
                return ParametricEqConfigureStatus::gain_out_of_range;
            if (band.q < minimum_q || band.q > maximum_q)
                return ParametricEqConfigureStatus::q_out_of_range;
            has_solo = has_solo || band.solo;
            previous_frequency = band.frequency_hz;
        }
        for (const Band band : bands) {
            if (!band.enabled || (has_solo && !band.solo)) continue;
            selected[selected_count] = band;
            candidate[selected_count] = design_band(band);
            if (!finite(candidate[selected_count])) return ParametricEqConfigureStatus::non_finite;
            if (!biquad_is_stable(candidate[selected_count]))
                return ParametricEqConfigureStatus::unstable;
            ++selected_count;
        }

        const std::size_t staging = 1 - active_index_;
        const auto installed = cascades_[staging].set_coefficients(
            std::span<const Coefficients>(candidate.data(), selected_count),
            SosCascadeTransition::reset_state);
        if (installed == SosCascadeInstallStatus::not_prepared)
            return ParametricEqConfigureStatus::not_prepared;
        if (installed == SosCascadeInstallStatus::over_capacity)
            return ParametricEqConfigureStatus::over_capacity;
        if (installed == SosCascadeInstallStatus::non_finite)
            return ParametricEqConfigureStatus::non_finite;
        if (installed == SosCascadeInstallStatus::unstable)
            return ParametricEqConfigureStatus::unstable;

        requested_bands_ = selected;
        requested_coefficients_ = candidate;
        requested_count_ = selected_count;
        if (transition_samples == 0) {
            active_index_ = staging;
            active_count_ = selected_count;
            active_bands_ = selected;
            cascades_[1 - active_index_].reset();
        } else {
            transition_total_ = transition_samples;
            transition_remaining_ = transition_samples;
        }
        return ParametricEqConfigureStatus::configured;
    }

    SampleType process(SampleType input) noexcept {
        if (!prepared()) return input;
        const SampleType from = cascades_[active_index_].process(input);
        if (!transitioning()) return std::isfinite(from) ? from : SampleType{};
        const SampleType to = cascades_[1 - active_index_].process(input);
        const std::size_t completed = transition_total_ - transition_remaining_ + 1;
        const SampleType mix = static_cast<SampleType>(completed) /
                               static_cast<SampleType>(transition_total_);
        const SampleType output = transition_remaining_ == 1 ? to : std::lerp(from, to, mix);
        if (--transition_remaining_ == 0) {
            active_index_ = 1 - active_index_;
            active_count_ = requested_count_;
            active_bands_ = requested_bands_;
            cascades_[1 - active_index_].reset();
        }
        return std::isfinite(output) ? output : SampleType{};
    }

    bool process_block(SampleType* samples, std::size_t frames) noexcept {
        if (samples == nullptr && frames != 0) return false;
        for (std::size_t i = 0; i < frames; ++i) samples[i] = process(samples[i]);
        return true;
    }

    void reset() noexcept {
        cascades_[0].reset();
        cascades_[1].reset();
        if (transitioning()) {
            active_index_ = 1 - active_index_;
            active_count_ = requested_count_;
            active_bands_ = requested_bands_;
        }
        transition_total_ = transition_remaining_ = 0;
    }

    bool prepared() const noexcept { return capacity_ != 0; }
    bool transitioning() const noexcept { return transition_remaining_ != 0; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t band_count() const noexcept { return requested_count_; }
    SampleType sample_rate() const noexcept { return sample_rate_; }
    static constexpr std::size_t storage_capacity() noexcept { return MaxBands; }
    Band band(std::size_t index) const noexcept {
        return index < requested_count_ ? requested_bands_[index] : Band{};
    }
    Coefficients coefficients(std::size_t index) const noexcept {
        return index < requested_count_ ? requested_coefficients_[index] : Coefficients{};
    }

    /// Stationary response of the requested endpoint, in linear magnitude.
    double magnitude(double frequency_hz) const noexcept {
        if (!prepared() || !std::isfinite(frequency_hz) || frequency_hz < 0.0 ||
            frequency_hz > static_cast<double>(sample_rate_) * 0.5)
            return std::numeric_limits<double>::quiet_NaN();
        return cascade_magnitude(std::span<const Coefficients>(
                                     requested_coefficients_.data(), requested_count_),
                                 angular_frequency(frequency_hz, static_cast<double>(sample_rate_)));
    }
    float magnitude_db(double frequency_hz) const noexcept {
        return magnitude_to_db(magnitude(frequency_hz));
    }
    void response_curve_db(double min_hz, double max_hz, std::span<float> out) const noexcept {
        response_curve_db(std::span<const Coefficients>(requested_coefficients_.data(), requested_count_),
                          min_hz, max_hz, static_cast<double>(sample_rate_), out);
    }

  private:
    static bool finite(const Coefficients& c) noexcept {
        return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) &&
               std::isfinite(c.a1) && std::isfinite(c.a2);
    }
    SampleType supported_frequency_ceiling_hz() const noexcept {
        return std::min(maximum_frequency_hz, sample_rate_ * SampleType{0.49});
    }
    static Coefficients design_band(const Band& band, SampleType sample_rate) noexcept {
        BiquadT<double> designer;
        const auto type = band.type == ParametricEqBandType::low_shelf
                              ? BiquadT<double>::Type::low_shelf
                              : band.type == ParametricEqBandType::high_shelf
                                    ? BiquadT<double>::Type::high_shelf
                                    : BiquadT<double>::Type::peaking;
        designer.set_coefficients(type, static_cast<double>(band.frequency_hz),
                                  static_cast<double>(band.q), static_cast<double>(sample_rate),
                                  static_cast<double>(band.gain_db));
        const auto c = designer.coefficients();
        return {static_cast<SampleType>(c.b0), static_cast<SampleType>(c.b1),
                static_cast<SampleType>(c.b2), static_cast<SampleType>(c.a1),
                static_cast<SampleType>(c.a2)};
    }
    Coefficients design_band(const Band& band) const noexcept {
        return design_band(band, sample_rate_);
    }

    std::array<SosCascadeT<SampleType, MaxBands>, 2> cascades_{};
    std::array<Band, MaxBands> requested_bands_{};
    std::array<Band, MaxBands> active_bands_{};
    std::array<Coefficients, MaxBands> requested_coefficients_{};
    SampleType sample_rate_ = SampleType{};
    std::size_t capacity_ = 0;
    std::size_t active_index_ = 0;
    std::size_t active_count_ = 0;
    std::size_t requested_count_ = 0;
    std::size_t transition_total_ = 0;
    std::size_t transition_remaining_ = 0;
};

using ParametricEq = ParametricEqT<float>;
using ParametricEq64 = ParametricEqT<double>;

} // namespace pulp::signal
