#pragma once

/// @file filter_morph.hpp
/// Stable, fixed-state morphing between two canonical second-order filters.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/frequency_response.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class MorphFilterType { lowpass, bandpass, highpass, notch };

template <typename SampleType = float> class FilterMorphT {
    static_assert(std::is_floating_point_v<SampleType>);
    using InternalType = std::conditional_t<std::is_same_v<SampleType, float>, double, SampleType>;

  public:
    static constexpr SampleType min_frequency_hz = SampleType{20};
    static constexpr SampleType min_q = SampleType{0.1};
    static constexpr SampleType max_q = SampleType{20};
    static constexpr SampleType max_sample_rate = SampleType{384000};

    struct Endpoint {
        MorphFilterType type = MorphFilterType::lowpass;
        SampleType frequency_hz = SampleType{1000};
        SampleType q = SampleType{0.7071067811865476};
    };

    /// Transactionally design both endpoints. Invalid input leaves the live
    /// filters and their state untouched. Call only at a block boundary.
    bool configure(SampleType sample_rate, Endpoint first, Endpoint second) noexcept {
        if (!valid_sample_rate_(sample_rate) || !valid_endpoint_(first, sample_rate) ||
            !valid_endpoint_(second, sample_rate))
            return false;

        const auto first_coefficients = design_(first, sample_rate);
        const auto second_coefficients = design_(second, sample_rate);
        if (!finite_coefficients_(first_coefficients) ||
            !finite_coefficients_(second_coefficients) || !biquad_is_stable(first_coefficients) ||
            !biquad_is_stable(second_coefficients))
            return false;

        first_filter_.set_coefficients(first_coefficients);
        second_filter_.set_coefficients(second_coefficients);
        sample_rate_ = sample_rate;
        endpoints_[0] = first;
        endpoints_[1] = second;
        configured_ = true;
        return true;
    }

    /// Set the linear-amplitude morph position. A convex output blend avoids
    /// hidden midpoint gain and keeps the canonical endpoint normalizations.
    bool set_morph(SampleType amount) noexcept {
        if (!std::isfinite(static_cast<double>(amount)) || amount < SampleType{0} ||
            amount > SampleType{1})
            return false;
        morph_ = amount;
        return true;
    }

    SampleType process(SampleType input) noexcept {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        if (!configured_)
            return input;

        // Always advance both paths, including at exact endpoints, so a later
        // morph change is independent of the preceding block partition.
        const InternalType first = first_filter_.process(static_cast<InternalType>(input));
        const InternalType second = second_filter_.process(static_cast<InternalType>(input));
        const bool first_finite = std::isfinite(first);
        const bool second_finite = std::isfinite(second);
        if (morph_ == SampleType{0}) {
            if (!second_finite)
                second_filter_.reset();
            if (!first_finite || !representable_(first)) {
                first_filter_.reset();
                return SampleType{0};
            }
            return static_cast<SampleType>(first);
        }
        if (morph_ == SampleType{1}) {
            if (!first_finite)
                first_filter_.reset();
            if (!second_finite || !representable_(second)) {
                second_filter_.reset();
                return SampleType{0};
            }
            return static_cast<SampleType>(second);
        }
        if (!first_finite || !second_finite) {
            if (!first_finite)
                first_filter_.reset();
            if (!second_finite)
                second_filter_.reset();
            return SampleType{0};
        }
        const InternalType output = std::lerp(first, second, static_cast<InternalType>(morph_));
        if (!representable_(output)) {
            reset();
            return SampleType{0};
        }
        return static_cast<SampleType>(output);
    }

    bool process_block(const SampleType* input, SampleType* output, std::size_t frames) noexcept {
        if ((input == nullptr || output == nullptr) && frames != 0)
            return false;
        for (std::size_t i = 0; i < frames; ++i)
            output[i] = process(input[i]);
        return true;
    }

    bool process_block(SampleType* samples, std::size_t frames) noexcept {
        return process_block(samples, samples, frames);
    }

    void reset() noexcept {
        first_filter_.reset();
        second_filter_.reset();
    }

    [[nodiscard]] Endpoint first_endpoint() const noexcept {
        return endpoints_[0];
    }
    [[nodiscard]] Endpoint second_endpoint() const noexcept {
        return endpoints_[1];
    }
    [[nodiscard]] SampleType morph() const noexcept {
        return morph_;
    }
    [[nodiscard]] SampleType sample_rate() const noexcept {
        return sample_rate_;
    }
    [[nodiscard]] bool configured() const noexcept {
        return configured_;
    }
    [[nodiscard]] BiquadCoefficientsT<SampleType> first_coefficients() const noexcept {
        return narrow_coefficients_(first_filter_.coefficients());
    }
    [[nodiscard]] BiquadCoefficientsT<SampleType> second_coefficients() const noexcept {
        return narrow_coefficients_(second_filter_.coefficients());
    }

    /// Exact complex parallel sum of the two live sections.
    [[nodiscard]] double magnitude(double frequency_hz) const noexcept {
        if (!configured_)
            return 1.0;
        const double omega = angular_frequency(frequency_hz, static_cast<double>(sample_rate_));
        const auto first = complex_response_(first_filter_.coefficients(), omega);
        const auto second = complex_response_(second_filter_.coefficients(), omega);
        return std::abs(first + static_cast<double>(morph_) * (second - first));
    }

    [[nodiscard]] float magnitude_db(double frequency_hz) const noexcept {
        return magnitude_to_db(magnitude(frequency_hz));
    }

    void response_curve_db(double min_hz, double max_hz, std::span<float> output) const noexcept {
        for (std::size_t i = 0; i < output.size(); ++i)
            output[i] = magnitude_db(log_frequency_at(i, output.size(), min_hz, max_hz));
    }

    static constexpr int latency_samples() noexcept {
        return 0;
    }
    /// Recursive biquads have an asymptotic (formally unbounded) tail.
    static constexpr int tail_samples() noexcept {
        return -1;
    }

  private:
    static bool valid_sample_rate_(SampleType sample_rate) noexcept {
        return std::isfinite(static_cast<double>(sample_rate)) &&
               sample_rate > min_frequency_hz * SampleType{2.1} && sample_rate <= max_sample_rate;
    }

    static bool valid_endpoint_(const Endpoint& endpoint, SampleType sample_rate) noexcept {
        if (!std::isfinite(static_cast<double>(endpoint.frequency_hz)) ||
            !std::isfinite(static_cast<double>(endpoint.q)) ||
            endpoint.frequency_hz < min_frequency_hz ||
            endpoint.frequency_hz > sample_rate * SampleType{0.45} || endpoint.q < min_q ||
            endpoint.q > max_q)
            return false;
        switch (endpoint.type) {
        case MorphFilterType::lowpass:
        case MorphFilterType::bandpass:
        case MorphFilterType::highpass:
        case MorphFilterType::notch:
            return true;
        }
        return false;
    }

    static BiquadCoefficientsT<SampleType> design_(Endpoint endpoint,
                                                   SampleType sample_rate) noexcept {
        // Design in double even for FilterMorph<float>. At the documented
        // 20 Hz / high-rate corner, float cos(w) can round to one and erase a
        // low-pass numerator before normalization.
        BiquadT<double> designer;
        designer.set_coefficients(
            to_biquad_type_double_(endpoint.type), static_cast<double>(endpoint.frequency_hz),
            static_cast<double>(endpoint.q), static_cast<double>(sample_rate));
        const auto c = designer.coefficients();
        return {static_cast<SampleType>(c.b0), static_cast<SampleType>(c.b1),
                static_cast<SampleType>(c.b2), static_cast<SampleType>(c.a1),
                static_cast<SampleType>(c.a2)};
    }

    static BiquadT<double>::Type to_biquad_type_double_(MorphFilterType type) noexcept {
        switch (type) {
        case MorphFilterType::lowpass:
            return BiquadT<double>::Type::lowpass;
        case MorphFilterType::bandpass:
            return BiquadT<double>::Type::bandpass;
        case MorphFilterType::highpass:
            return BiquadT<double>::Type::highpass;
        case MorphFilterType::notch:
            return BiquadT<double>::Type::notch;
        }
        return BiquadT<double>::Type::lowpass;
    }

    static bool finite_coefficients_(const BiquadCoefficientsT<SampleType>& c) noexcept {
        return std::isfinite(static_cast<double>(c.b0)) &&
               std::isfinite(static_cast<double>(c.b1)) &&
               std::isfinite(static_cast<double>(c.b2)) &&
               std::isfinite(static_cast<double>(c.a1)) && std::isfinite(static_cast<double>(c.a2));
    }

    template <typename CoefficientType>
    static std::complex<double> complex_response_(const BiquadCoefficientsT<CoefficientType>& c,
                                                  double omega) noexcept {
        const auto z1 = std::polar(1.0, -omega);
        const auto z2 = z1 * z1;
        const std::complex<double> numerator = static_cast<double>(c.b0) +
                                               static_cast<double>(c.b1) * z1 +
                                               static_cast<double>(c.b2) * z2;
        const std::complex<double> denominator =
            1.0 + static_cast<double>(c.a1) * z1 + static_cast<double>(c.a2) * z2;
        if (!(std::abs(denominator) > 0.0))
            return {};
        return numerator / denominator;
    }

    static bool representable_(InternalType value) noexcept {
        return std::isfinite(value) &&
               std::abs(value) <= static_cast<InternalType>(std::numeric_limits<SampleType>::max());
    }

    static BiquadCoefficientsT<SampleType>
    narrow_coefficients_(const BiquadCoefficientsT<InternalType>& c) noexcept {
        return {static_cast<SampleType>(c.b0), static_cast<SampleType>(c.b1),
                static_cast<SampleType>(c.b2), static_cast<SampleType>(c.a1),
                static_cast<SampleType>(c.a2)};
    }

    BiquadT<InternalType> first_filter_{};
    BiquadT<InternalType> second_filter_{};
    Endpoint endpoints_[2]{};
    SampleType sample_rate_ = SampleType{48000};
    SampleType morph_ = SampleType{0};
    bool configured_ = false;
};

using FilterMorph = FilterMorphT<float>;
using FilterMorph64 = FilterMorphT<double>;

} // namespace pulp::signal
