#pragma once

/// @file comb_filter.hpp
/// Prepared feedforward, feedback, and Schroeder allpass comb filters.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fractional_delay.hpp>

#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <optional>
#include <type_traits>

namespace pulp::signal {

enum class CombFilterMode { feedforward, feedback, allpass };

inline constexpr bool is_valid_comb_filter_mode(CombFilterMode mode) noexcept {
    return mode == CombFilterMode::feedforward || mode == CombFilterMode::feedback ||
           mode == CombFilterMode::allpass;
}

struct CombFilterConfig {
    CombFilterMode mode = CombFilterMode::feedforward;
    std::size_t delay_samples = 2;
    double gain = 0.0;
};

enum class CombFilterStatus {
    ok,
    not_prepared,
    not_configured,
    invalid_argument,
    non_finite_input,
    output_out_of_range,
};

template <typename SampleType> struct CombFilterSampleResult {
    SampleType sample{};
    CombFilterStatus status = CombFilterStatus::not_prepared;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == CombFilterStatus::ok;
    }
};

struct CombFilterBlockResult {
    CombFilterStatus status = CombFilterStatus::ok;
    std::size_t processed_frames = 0;
    std::size_t fault_count = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == CombFilterStatus::ok;
    }
};

/// Returns H(e^jw) for the documented mode equations. Invalid arguments return
/// a complex NaN. `omega` is radians/sample.
inline std::complex<double> comb_filter_response(CombFilterMode mode,
                                                 std::size_t delay_samples, double gain,
                                                 double omega) noexcept {
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    if (!is_valid_comb_filter_mode(mode) || delay_samples < 2u || !std::isfinite(gain) ||
        !std::isfinite(omega) || std::abs(gain) > 1.0 ||
        (mode != CombFilterMode::feedforward &&
         std::abs(gain) > 0.999))
        return {nan, nan};

    const auto delayed = std::polar(1.0, -omega * static_cast<double>(delay_samples));
    if (mode == CombFilterMode::feedforward)
        return 1.0 + gain * delayed;
    const auto denominator = 1.0 - gain * delayed;
    if (mode == CombFilterMode::feedback)
        return 1.0 / denominator;
    return (delayed - gain) / denominator;
}

/// A fixed-capacity, integer-delay comb-filter family.
///
/// The modes implement:
///   feedforward: y[n] = x[n] + g x[n-D]
///   feedback:    y[n] = x[n] + g y[n-D]
///   allpass:     y[n] = w[n-D] - g w[n], w[n] = x[n] + g w[n-D]
///
/// prepare() is the only allocating operation and is transactional. configure()
/// validates the complete candidate before committing it, then starts from a
/// cold history. Processing, configure(), reset(), and fault recovery allocate
/// nothing. Exact in-place block processing is supported. A nonfinite input or
/// arithmetic overflow emits zero and discards recursive history in O(1).
template <typename SampleType = float> class CombFilterT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    static constexpr double maximum_recursive_gain = 0.999;
    static constexpr std::size_t minimum_delay_samples = 2u;

    [[nodiscard]] bool prepare(std::size_t maximum_delay_samples) {
        if (!history_.prepare(maximum_delay_samples))
            return false;
        maximum_delay_samples_ = maximum_delay_samples;
        prepared_ = true;
        configured_ = false;
        fault_count_ = 0;
        return true;
    }

    [[nodiscard]] bool configure(const CombFilterConfig& candidate) noexcept {
        if (!prepared_ || !valid_config(candidate))
            return false;
        config_ = candidate;
        configured_ = true;
        history_.discard_history();
        return true;
    }

    void reset() noexcept {
        history_.reset();
        fault_count_ = 0;
    }

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }
    [[nodiscard]] bool configured() const noexcept { return configured_; }
    [[nodiscard]] std::size_t maximum_delay_samples() const noexcept {
        return prepared_ ? maximum_delay_samples_ : 0u;
    }
    [[nodiscard]] const CombFilterConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::size_t fault_count() const noexcept { return fault_count_; }
    void clear_fault_count() noexcept { fault_count_ = 0; }

    /// Comb structures retain a direct path and therefore add no fixed host
    /// compensation latency. D is an internal signal-path delay.
    [[nodiscard]] static constexpr std::size_t processing_latency_samples() noexcept {
        return 0u;
    }

    /// Finite tail in samples, or nullopt for a recursive infinite tail.
    /// Zero-gain feedback is dry; zero-gain allpass is a pure D-sample delay.
    [[nodiscard]] std::optional<std::size_t> tail_samples() const noexcept {
        if (!configured_)
            return std::nullopt;
        if (config_.mode == CombFilterMode::feedforward)
            return config_.gain == 0.0 ? 0u : config_.delay_samples;
        if (config_.mode == CombFilterMode::feedback)
            return config_.gain == 0.0 ? std::optional<std::size_t>{0u} : std::nullopt;
        return config_.gain == 0.0 ? std::optional{config_.delay_samples} : std::nullopt;
    }

    [[nodiscard]] CombFilterSampleResult<SampleType> process(SampleType input) noexcept {
        if (!prepared_)
            return {{}, CombFilterStatus::not_prepared};
        if (!configured_)
            return {{}, CombFilterStatus::not_configured};
        if (!std::isfinite(input))
            return fail(CombFilterStatus::non_finite_input);

        const auto delayed = history_.read_lagrange3_at(
            static_cast<double>(config_.delay_samples));
        if (!delayed)
            return fail(CombFilterStatus::output_out_of_range);

        const auto x = static_cast<long double>(input);
        const auto d = static_cast<long double>(delayed.sample);
        const auto g = static_cast<long double>(config_.gain);
        long double output = 0.0L;
        long double stored = 0.0L;
        if (config_.mode == CombFilterMode::feedforward) {
            output = x + g * d;
            stored = x;
        } else if (config_.mode == CombFilterMode::feedback) {
            output = x + g * d;
            stored = output;
        } else {
            stored = x + g * d;
            output = d - g * stored;
        }

        const auto limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
        if (!std::isfinite(output) || !std::isfinite(stored) || std::abs(output) > limit ||
            std::abs(stored) > limit)
            return fail(CombFilterStatus::output_out_of_range);
        // Snap the recursive state to zero so a decaying tail does not park the
        // feedback and allpass paths in denormal arithmetic after input stops.
        if (history_.push(snap_to_zero(static_cast<SampleType>(stored))) != FractionalDelayStatus::ok)
            return fail(CombFilterStatus::output_out_of_range);
        return {static_cast<SampleType>(output), CombFilterStatus::ok};
    }

    [[nodiscard]] CombFilterBlockResult process(const SampleType* input, SampleType* output,
                                                std::size_t frames) noexcept {
        if (frames == 0u)
            return {};
        if (input == nullptr || output == nullptr)
            return {CombFilterStatus::invalid_argument, 0u, 0u};
        if (!prepared_)
            return {CombFilterStatus::not_prepared, 0u, 0u};
        if (!configured_)
            return {CombFilterStatus::not_configured, 0u, 0u};

        CombFilterBlockResult result{};
        for (std::size_t i = 0; i < frames; ++i) {
            const auto current_input = input[i];
            const auto current = process(current_input);
            output[i] = current.sample;
            ++result.processed_frames;
            if (!current) {
                if (result.status == CombFilterStatus::ok)
                    result.status = current.status;
                ++result.fault_count;
            }
        }
        return result;
    }

  private:
    [[nodiscard]] bool valid_config(const CombFilterConfig& candidate) const noexcept {
        if (!is_valid_comb_filter_mode(candidate.mode) ||
            candidate.delay_samples < minimum_delay_samples ||
            candidate.delay_samples > maximum_delay_samples_ || !std::isfinite(candidate.gain))
            return false;
        if (candidate.mode == CombFilterMode::feedforward)
            return std::abs(candidate.gain) <= 1.0;
        return std::abs(candidate.gain) <= maximum_recursive_gain;
    }

    [[nodiscard]] CombFilterSampleResult<SampleType> fail(CombFilterStatus status) noexcept {
        history_.discard_history();
        ++fault_count_;
        return {{}, status};
    }

    FractionalDelayHistoryT<SampleType> history_{};
    CombFilterConfig config_{};
    std::size_t maximum_delay_samples_ = 0;
    std::size_t fault_count_ = 0;
    bool prepared_ = false;
    bool configured_ = false;
};

using CombFilter = CombFilterT<float>;
using CombFilter64 = CombFilterT<double>;

} // namespace pulp::signal
