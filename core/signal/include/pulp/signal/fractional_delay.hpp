#pragma once

/// @file fractional_delay.hpp
/// Prepared fractional-delay storage with Thiran-1 and Lagrange-3/5 reads.

#include <pulp/signal/interpolator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

enum class FractionalDelayMethod {
    thiran1,
    lagrange3,
    lagrange5,
};

inline constexpr bool is_valid_fractional_delay_method(FractionalDelayMethod method) noexcept {
    return method == FractionalDelayMethod::thiran1 || method == FractionalDelayMethod::lagrange3 ||
           method == FractionalDelayMethod::lagrange5;
}

enum class FractionalDelayStatus {
    ok,
    not_prepared,
    invalid_argument,
    non_finite_input,
    invalid_delay,
    output_out_of_range,
};

struct Thiran1Coefficients {
    double delay_samples = 1.0;
    double feedback = 0.0;
    bool valid = false;
};

/// Designs H(z) = (a + z^-1) / (1 + a z^-1), where
/// a = (1 - D) / (1 + D). D in [1, 2] keeps the pole radius at most 1/3.
inline Thiran1Coefficients design_thiran1(double delay_samples) noexcept {
    if (!std::isfinite(delay_samples) || delay_samples < 1.0 || delay_samples > 2.0)
        return {};
    return {.delay_samples = delay_samples,
            .feedback = (1.0 - delay_samples) / (1.0 + delay_samples),
            .valid = true};
}

inline double thiran1_magnitude(const Thiran1Coefficients& design, double omega) noexcept {
    if (!design.valid || !std::isfinite(omega))
        return std::numeric_limits<double>::quiet_NaN();
    return 1.0;
}

/// Exact group delay of the first-order allpass at angular frequency omega.
inline double thiran1_group_delay_samples(const Thiran1Coefficients& design,
                                          double omega) noexcept {
    if (!design.valid || !std::isfinite(omega))
        return std::numeric_limits<double>::quiet_NaN();
    const auto a = design.feedback;
    const auto denominator = 1.0 + a * a + 2.0 * a * std::cos(omega);
    if (!(denominator > 0.0) || !std::isfinite(denominator))
        return std::numeric_limits<double>::quiet_NaN();
    return (1.0 - a * a) / denominator;
}

inline std::array<double, 6> lagrange5_weights(double fraction) noexcept {
    std::array<double, 6> weights{};
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
        weights.fill(std::numeric_limits<double>::quiet_NaN());
        return weights;
    }
    constexpr std::array<double, 6> nodes{-2.0, -1.0, 0.0, 1.0, 2.0, 3.0};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        double weight = 1.0;
        for (std::size_t j = 0; j < nodes.size(); ++j) {
            if (i != j)
                weight *= (fraction - nodes[j]) / (nodes[i] - nodes[j]);
        }
        weights[i] = weight;
    }
    return weights;
}

template <typename SampleType>
SampleType lagrange5(double fraction, SampleType ym2, SampleType ym1, SampleType y0, SampleType y1,
                     SampleType y2, SampleType y3) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    const auto weights = lagrange5_weights(fraction);
    const std::array<SampleType, 6> samples{ym2, ym1, y0, y1, y2, y3};
    SampleType output{};
    for (std::size_t i = 0; i < samples.size(); ++i)
        output += static_cast<SampleType>(weights[i]) * samples[i];
    return output;
}

namespace detail {

struct FractionalDelaySplit {
    std::size_t integer = 0;
    double fraction = 0.0;
    double canonical = 0.0;
    bool valid = false;
};

[[nodiscard]] inline FractionalDelaySplit split_fractional_delay(double delay_samples) noexcept {
    if (!std::isfinite(delay_samples) || delay_samples < 0.0)
        return {};
    const auto nearest = std::round(delay_samples);
    const auto tolerance =
        8.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(delay_samples));
    const auto canonical = std::abs(delay_samples - nearest) <= tolerance ? nearest : delay_samples;
    if (canonical < 0.0)
        return {};
    constexpr auto size_digits = std::numeric_limits<std::size_t>::digits;
    constexpr auto double_digits = std::numeric_limits<double>::digits;
    const auto size_limit = static_cast<double>(std::numeric_limits<std::size_t>::max());
    if constexpr (size_digits <= double_digits) {
        if (canonical > size_limit)
            return {};
    } else {
        // The cast to double rounds SIZE_MAX up to the next power of two.
        // Reject that rounded endpoint before converting back to size_t.
        if (canonical >= size_limit)
            return {};
    }
    const auto integer_value = std::floor(canonical);
    return {.integer = static_cast<std::size_t>(integer_value),
            .fraction = canonical - integer_value,
            .canonical = canonical,
            .valid = true};
}

} // namespace detail

template <typename SampleType> struct FractionalDelaySampleResult {
    SampleType sample{};
    FractionalDelayStatus status = FractionalDelayStatus::not_prepared;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == FractionalDelayStatus::ok;
    }
};

/// Prepared history shared by any number of stateless Lagrange read heads.
///
/// Pushing and reading are deliberately separate operations. A caller may read
/// the current immutable history before pushing a feedback result, or push an
/// input first and then take any number of simultaneous reads. Delays are
/// measured behind the next write: delay 1 is the most recently pushed sample.
/// Failed prepare calls leave the previous history and configuration untouched.
template <typename SampleType = float> class FractionalDelayHistoryT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    FractionalDelayHistoryT() = default;
    FractionalDelayHistoryT(const FractionalDelayHistoryT&) = delete;
    FractionalDelayHistoryT& operator=(const FractionalDelayHistoryT&) = delete;

    FractionalDelayHistoryT(FractionalDelayHistoryT&& other) noexcept {
        move_from(std::move(other));
    }

    FractionalDelayHistoryT& operator=(FractionalDelayHistoryT&& other) noexcept {
        if (this != &other)
            move_from(std::move(other));
        return *this;
    }

    [[nodiscard]] bool prepare(std::size_t maximum_delay_samples) {
        constexpr auto additional_history = 3u;
        if (maximum_delay_samples < minimum_delay_samples(FractionalDelayMethod::lagrange3) ||
            maximum_delay_samples > std::numeric_limits<std::size_t>::max() - additional_history)
            return false;

        const auto storage_size = maximum_delay_samples + additional_history;
        std::vector<SampleType> replacement;
        if (storage_size > replacement.max_size()) {
            return false;
        }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            replacement.assign(storage_size, SampleType{});
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
#endif

        buffer_.swap(replacement);
        maximum_delay_samples_ = maximum_delay_samples;
        write_ = 0;
        valid_ = 0;
        prepared_ = true;
        return true;
    }

    void reset() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), SampleType{});
        write_ = 0;
        valid_ = 0;
    }

    /// Forgets all prior samples in constant time. Unlike reset(), this does
    /// not eagerly clear storage; invalidated slots remain unreachable until
    /// they have been overwritten by later pushes.
    void discard_history() noexcept {
        write_ = 0;
        valid_ = 0;
    }

    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] std::size_t maximum_delay_samples() const noexcept {
        return prepared_ ? maximum_delay_samples_ : 0u;
    }
    [[nodiscard]] std::size_t retained_samples() const noexcept {
        return buffer_.size();
    }
    [[nodiscard]] std::size_t retained_bytes() const noexcept {
        return buffer_.size() * sizeof(SampleType);
    }
    [[nodiscard]] std::size_t required_older_lookback() const noexcept {
        return prepared_ ? maximum_delay_samples_ + 3u : 0u;
    }

    [[nodiscard]] static constexpr std::size_t
    minimum_delay_samples(FractionalDelayMethod method) noexcept {
        if (method == FractionalDelayMethod::lagrange3)
            return 2u;
        if (method == FractionalDelayMethod::lagrange5)
            return 3u;
        return 0u;
    }

    /// Advances history by one sample. Nonfinite input advances with zero and
    /// reports the fault so invalid values can never poison later read heads.
    [[nodiscard]] FractionalDelayStatus push(SampleType sample) noexcept {
        if (!prepared_)
            return FractionalDelayStatus::not_prepared;
        const auto status = std::isfinite(sample) ? FractionalDelayStatus::ok
                                                  : FractionalDelayStatus::non_finite_input;
        buffer_[write_] = status == FractionalDelayStatus::ok ? sample : SampleType{};
        write_ = (write_ + 1u) % buffer_.size();
        valid_ = std::min(valid_ + 1u, buffer_.size());
        return status;
    }

    /// Reads one stateless head from the current history snapshot. Only the two
    /// Lagrange methods are accepted; reads never advance or alter history.
    [[nodiscard]] FractionalDelaySampleResult<SampleType>
    read_at(double delay_samples, FractionalDelayMethod method) const noexcept {
        if (!prepared_)
            return {{}, FractionalDelayStatus::not_prepared};
        const auto minimum = minimum_delay_samples(method);
        if (minimum == 0u)
            return {{}, FractionalDelayStatus::invalid_argument};

        const auto split = detail::split_fractional_delay(delay_samples);
        if (!split.valid || split.canonical < static_cast<double>(minimum) ||
            split.canonical > static_cast<double>(maximum_delay_samples_))
            return {{}, FractionalDelayStatus::invalid_delay};

        long double output = 0.0L;
        if (method == FractionalDelayMethod::lagrange3) {
            output = static_cast<long double>(Interpolator::lagrange<long double>(
                static_cast<long double>(split.fraction), tap_delay(split.integer - 1u),
                tap_delay(split.integer), tap_delay(split.integer + 1u),
                tap_delay(split.integer + 2u)));
        } else {
            output = lagrange5<long double>(
                split.fraction, tap_delay(split.integer - 2u), tap_delay(split.integer - 1u),
                tap_delay(split.integer), tap_delay(split.integer + 1u),
                tap_delay(split.integer + 2u), tap_delay(split.integer + 3u));
        }

        const auto limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
        if (!std::isfinite(output) || output > limit || output < -limit)
            return {{}, FractionalDelayStatus::output_out_of_range};
        return {static_cast<SampleType>(output), FractionalDelayStatus::ok};
    }

    [[nodiscard]] FractionalDelaySampleResult<SampleType>
    read_lagrange3_at(double delay_samples) const noexcept {
        return read_at(delay_samples, FractionalDelayMethod::lagrange3);
    }

    [[nodiscard]] FractionalDelaySampleResult<SampleType>
    read_lagrange5_at(double delay_samples) const noexcept {
        return read_at(delay_samples, FractionalDelayMethod::lagrange5);
    }

  private:
    void move_from(FractionalDelayHistoryT&& other) noexcept {
        buffer_ = std::move(other.buffer_);
        write_ = std::exchange(other.write_, 0u);
        valid_ = std::exchange(other.valid_, 0u);
        maximum_delay_samples_ = std::exchange(other.maximum_delay_samples_, 0u);
        prepared_ = std::exchange(other.prepared_, false);
        // Vector's moved-from size is valid but otherwise unspecified. Clear it
        // so every moved-from query observes the same unprepared empty state.
        other.buffer_.clear();
    }

    [[nodiscard]] long double tap_delay(std::size_t delay) const noexcept {
        if (delay == 0u || delay > valid_)
            return 0.0L;
        const auto index = (write_ + buffer_.size() - delay) % buffer_.size();
        return static_cast<long double>(buffer_[index]);
    }

    std::vector<SampleType> buffer_{};
    std::size_t write_ = 0;
    std::size_t valid_ = 0;
    std::size_t maximum_delay_samples_ = 0;
    bool prepared_ = false;
};

struct FractionalDelayBlockResult {
    FractionalDelayStatus status = FractionalDelayStatus::not_prepared;
    std::size_t processed_frames = 0;
    std::size_t fault_count = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == FractionalDelayStatus::ok;
    }
};

/// A prepared mono fractional-delay line.
///
/// `prepare()` is the only allocating operation and is transactional. Processing
/// first pushes the current input, then reads the requested causal delay. Exact
/// in-place block processing is supported. The method is fixed until the next
/// successful prepare. Lagrange delay may cross integer boundaries; Thiran-1
/// delay is confined to one prepared half-open integer interval. Neither path
/// applies hidden smoothing.
template <typename SampleType = float> class FractionalDelayLineT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    [[nodiscard]] bool prepare(std::size_t maximum_delay_samples, FractionalDelayMethod method) {
        return prepare_impl(maximum_delay_samples, method, 1u);
    }

    /// Prepares Thiran-1 for delays in [integer_interval_start,
    /// integer_interval_start + 1). The upper endpoint is not accepted.
    [[nodiscard]] bool prepare_thiran1(std::size_t maximum_delay_samples,
                                       std::size_t integer_interval_start) {
        return prepare_impl(maximum_delay_samples, FractionalDelayMethod::thiran1,
                            integer_interval_start);
    }

  private:
    [[nodiscard]] bool prepare_impl(std::size_t maximum_delay_samples, FractionalDelayMethod method,
                                    std::size_t thiran_integer_interval_start) {
        if (!is_valid_fractional_delay_method(method))
            return false;
        const auto minimum = minimum_delay_samples(method);
        const auto older = older_stencil_lookback(method);
        if (maximum_delay_samples < minimum ||
            maximum_delay_samples > std::numeric_limits<std::size_t>::max() - older - 1u)
            return false;
        if (method == FractionalDelayMethod::thiran1 &&
            (thiran_integer_interval_start < 1u ||
             thiran_integer_interval_start >= maximum_delay_samples))
            return false;

        const auto storage_size = maximum_delay_samples + older + 1u;
        std::vector<SampleType> replacement;
        if (storage_size > replacement.max_size()) {
            return false;
        }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            replacement.assign(storage_size, SampleType{});
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
#endif

        buffer_.swap(replacement);
        maximum_delay_samples_ = maximum_delay_samples;
        method_ = method;
        thiran_integer_interval_start_ = thiran_integer_interval_start;
        prepared_ = true;
        reset_state();
        return true;
    }

  public:
    void reset() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), SampleType{});
        reset_state();
    }

    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] std::optional<FractionalDelayMethod> method() const noexcept {
        return prepared_ ? std::optional{method_} : std::nullopt;
    }
    [[nodiscard]] std::size_t maximum_delay_samples() const noexcept {
        return prepared_ ? maximum_delay_samples_ : 0u;
    }
    [[nodiscard]] std::size_t minimum_delay_samples() const noexcept {
        return prepared_ ? minimum_delay_samples(method_) : 0u;
    }
    [[nodiscard]] std::optional<std::size_t> thiran_integer_interval_start() const noexcept {
        if (!prepared_ || method_ != FractionalDelayMethod::thiran1)
            return std::nullopt;
        return thiran_integer_interval_start_;
    }
    [[nodiscard]] std::size_t retained_samples() const noexcept {
        return buffer_.size();
    }
    [[nodiscard]] std::size_t required_older_lookback() const noexcept {
        return prepared_ ? maximum_delay_samples_ + older_stencil_lookback(method_) : 0u;
    }
    /// Processing itself adds no host-compensated latency. The requested delay
    /// is the signal-path delay and is available on the same process call.
    [[nodiscard]] static constexpr std::size_t processing_latency_samples() noexcept {
        return 0;
    }

    [[nodiscard]] static constexpr std::size_t
    minimum_delay_samples(FractionalDelayMethod method) noexcept {
        if (!is_valid_fractional_delay_method(method))
            return 0u;
        return method == FractionalDelayMethod::lagrange5 ? 2u : 1u;
    }

    [[nodiscard]] static constexpr std::size_t
    older_stencil_lookback(FractionalDelayMethod method) noexcept {
        if (!is_valid_fractional_delay_method(method))
            return 0u;
        if (method == FractionalDelayMethod::lagrange3)
            return 2u;
        if (method == FractionalDelayMethod::lagrange5)
            return 3u;
        return 0u;
    }

    [[nodiscard]] FractionalDelaySampleResult<SampleType> process(SampleType input,
                                                                  double delay_samples) noexcept {
        if (!prepared_)
            return {{}, FractionalDelayStatus::not_prepared};
        if (!std::isfinite(input)) {
            inject_zero();
            return {{}, FractionalDelayStatus::non_finite_input};
        }

        const auto split = detail::split_fractional_delay(delay_samples);
        const auto outside_general_range =
            !split.valid || split.canonical < static_cast<double>(minimum_delay_samples()) ||
            split.canonical > static_cast<double>(maximum_delay_samples_);
        const auto outside_thiran_interval =
            method_ == FractionalDelayMethod::thiran1 &&
            (!split.valid || split.integer != thiran_integer_interval_start_);
        if (outside_general_range || outside_thiran_interval) {
            inject_zero();
            return {{}, FractionalDelayStatus::invalid_delay};
        }

        push(input);
        long double output = 0.0L;
        if (method_ == FractionalDelayMethod::thiran1)
            output = read_thiran1(split.fraction);
        else if (method_ == FractionalDelayMethod::lagrange3)
            output = read_lagrange3(split.integer, split.fraction);
        else
            output = read_lagrange5(split.integer, split.fraction);

        const auto limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
        if (!std::isfinite(output) || output > limit || output < -limit) {
            replace_latest_with_zero();
            reset_recursive_state();
            return {{}, FractionalDelayStatus::output_out_of_range};
        }
        return {static_cast<SampleType>(output), FractionalDelayStatus::ok};
    }

    [[nodiscard]] FractionalDelayBlockResult process(const SampleType* input, SampleType* output,
                                                     std::size_t frames,
                                                     double delay_samples) noexcept {
        if (frames == 0)
            return {FractionalDelayStatus::ok, 0, 0};
        if (input == nullptr || output == nullptr)
            return {FractionalDelayStatus::invalid_argument, 0, 0};
        if (!prepared_)
            return {FractionalDelayStatus::not_prepared, 0, 0};
        return process_block(input, output, frames,
                             [delay_samples](std::size_t) noexcept { return delay_samples; });
    }

    [[nodiscard]] FractionalDelayBlockResult process(const SampleType* input, SampleType* output,
                                                     const double* delay_samples,
                                                     std::size_t frames) noexcept {
        if (frames == 0)
            return {FractionalDelayStatus::ok, 0, 0};
        if (input == nullptr || output == nullptr || delay_samples == nullptr)
            return {FractionalDelayStatus::invalid_argument, 0, 0};
        if (!prepared_)
            return {FractionalDelayStatus::not_prepared, 0, 0};
        return process_block(input, output, frames,
                             [delay_samples](std::size_t i) noexcept { return delay_samples[i]; });
    }

  private:
    template <typename DelayAt>
    FractionalDelayBlockResult process_block(const SampleType* input, SampleType* output,
                                             std::size_t frames, DelayAt&& delay_at) noexcept {
        FractionalDelayBlockResult result{FractionalDelayStatus::ok, 0, 0};
        for (std::size_t i = 0; i < frames; ++i) {
            const auto current_input = input[i];
            const auto current = process(current_input, delay_at(i));
            output[i] = current.sample;
            ++result.processed_frames;
            if (!current) {
                if (result.status == FractionalDelayStatus::ok)
                    result.status = current.status;
                ++result.fault_count;
            }
        }
        return result;
    }

    void reset_state() noexcept {
        write_ = 0;
        valid_ = 0;
        reset_recursive_state();
    }

    void reset_recursive_state() noexcept {
        thiran_input_ = 0.0L;
        thiran_output_ = 0.0L;
    }

    void push(SampleType sample) noexcept {
        buffer_[write_] = sample;
        write_ = (write_ + 1u) % buffer_.size();
        valid_ = std::min(valid_ + 1u, buffer_.size());
    }

    void inject_zero() noexcept {
        push(SampleType{});
        reset_recursive_state();
    }

    void replace_latest_with_zero() noexcept {
        const auto index = (write_ + buffer_.size() - 1u) % buffer_.size();
        buffer_[index] = SampleType{};
    }

    [[nodiscard]] long double tap(std::size_t delay) const noexcept {
        if (delay >= valid_)
            return 0.0L;
        const auto index = (write_ + buffer_.size() - 1u - delay) % buffer_.size();
        return static_cast<long double>(buffer_[index]);
    }

    [[nodiscard]] long double read_thiran1(double fraction) noexcept {
        const auto source = tap(thiran_integer_interval_start_ - 1u);
        const auto design = design_thiran1(1.0 + fraction);
        const auto a = static_cast<long double>(design.feedback);
        const auto output = a * source + thiran_input_ - a * thiran_output_;
        thiran_input_ = source;
        thiran_output_ = output;
        return output;
    }

    [[nodiscard]] long double read_lagrange3(std::size_t integer, double fraction) const noexcept {
        return static_cast<long double>(Interpolator::lagrange<long double>(
            static_cast<long double>(fraction), tap(integer - 1u), tap(integer), tap(integer + 1u),
            tap(integer + 2u)));
    }

    [[nodiscard]] long double read_lagrange5(std::size_t integer, double fraction) const noexcept {
        return lagrange5<long double>(fraction, tap(integer - 2u), tap(integer - 1u), tap(integer),
                                      tap(integer + 1u), tap(integer + 2u), tap(integer + 3u));
    }

    std::vector<SampleType> buffer_{};
    std::size_t write_ = 0;
    std::size_t valid_ = 0;
    std::size_t maximum_delay_samples_ = 0;
    std::size_t thiran_integer_interval_start_ = 1;
    FractionalDelayMethod method_ = FractionalDelayMethod::thiran1;
    long double thiran_input_ = 0.0L;
    long double thiran_output_ = 0.0L;
    bool prepared_ = false;
};

using FractionalDelayLine = FractionalDelayLineT<float>;
using FractionalDelayLine64 = FractionalDelayLineT<double>;
using FractionalDelayHistory = FractionalDelayHistoryT<float>;
using FractionalDelayHistory64 = FractionalDelayHistoryT<double>;

} // namespace pulp::signal
