#pragma once

/// @file waveguide_line.hpp
/// Prepared bidirectional traveling-wave delay rails.

#include <pulp/signal/fractional_delay.hpp>
#include <pulp/signal/smoothed_value.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>

namespace pulp::signal {

/// A passive bidirectional waveguide segment.
///
/// `left_in` travels toward the right port and emerges as `right_out`;
/// `right_in` travels toward the left port and emerges as `left_out`. Boundary
/// reflection and intentional loss belong outside this class. The two rails
/// use the shared prepared fractional-delay history, with double storage for
/// both float and double public I/O. Lagrange-5 retuning is unity gain at DC
/// and non-amplifying, but attenuates high frequencies at fractional delays;
/// it is therefore passive rather than mathematically lossless.
///
/// `prepare()` is transactional and is the only allocating operation. All
/// setters, `process()`, and `reset()` are bounded, lock-free, and allocation-
/// free. Delay changes glide for five milliseconds because the live shared
/// history exposes stateless Lagrange heads rather than a cross-interval
/// Thiran retune operation.
template <typename SampleType = float> class WaveguideLineT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    static constexpr double minimum_length_samples = 3.0;
    static constexpr double minimum_max_length_seconds = 0.001;
    static constexpr double maximum_max_length_seconds = 1.0;
    static constexpr double default_max_length_seconds = 0.05;
    static constexpr double retune_seconds = 0.005;

    [[nodiscard]] bool prepare(double sample_rate,
                               double max_length_seconds = default_max_length_seconds) {
        if (!std::isfinite(sample_rate) || !(sample_rate > 0.0) ||
            !std::isfinite(max_length_seconds) ||
            max_length_seconds < minimum_max_length_seconds ||
            max_length_seconds > maximum_max_length_seconds)
            return false;

        const auto requested = std::ceil(sample_rate * max_length_seconds);
        if (!std::isfinite(requested) || requested < minimum_length_samples ||
            requested >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
            return false;

        const auto maximum_delay = static_cast<std::size_t>(requested);
        FractionalDelayHistoryT<double> new_right_going;
        FractionalDelayHistoryT<double> new_left_going;
        if (!new_right_going.prepare(maximum_delay) || !new_left_going.prepare(maximum_delay))
            return false;

        right_going_ = std::move(new_right_going);
        left_going_ = std::move(new_left_going);
        sample_rate_ = sample_rate;
        maximum_length_samples_ = static_cast<double>(maximum_delay);
        target_length_samples_ = minimum_length_samples;
        length_.set_ramp_time(retune_seconds, sample_rate_);
        length_.set_immediate(target_length_samples_);
        length_configured_ = false;
        prepared_ = true;
        return true;
    }

    void set_length_samples(SampleType length) noexcept {
        if (!prepared_ || !std::isfinite(static_cast<double>(length)))
            return;
        target_length_samples_ = std::clamp(static_cast<double>(length), minimum_length_samples,
                                            maximum_length_samples_);
        if (!length_configured_) {
            length_.set_immediate(target_length_samples_);
            length_configured_ = true;
        } else {
            length_.set_target(target_length_samples_);
        }
    }

    [[nodiscard]] SampleType length_samples() const noexcept {
        return static_cast<SampleType>(target_length_samples_);
    }

    [[nodiscard]] double round_trip_seconds() const noexcept {
        return prepared_ ? 2.0 * target_length_samples_ / sample_rate_ : 0.0;
    }

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    /// Reads both boundary arrivals without advancing either rail.
    ///
    /// Feedback compositions should call `read_outputs()`, calculate every
    /// boundary reflection/scattering result, then call `push_inputs()` in the
    /// same frame. This two-phase seam avoids adding a hidden sample at every
    /// external boundary.
    void read_outputs(SampleType& left_out, SampleType& right_out) const noexcept {
        left_out = SampleType{};
        right_out = SampleType{};
        if (!prepared_)
            return;

        const auto delay = length_.current();
        const auto at_right = right_going_.read_lagrange5_at(delay);
        const auto at_left = left_going_.read_lagrange5_at(delay);
        if (at_right)
            right_out = finite_cast(at_right.sample);
        if (at_left)
            left_out = finite_cast(at_left.sample);
    }

    /// Injects both boundary results and advances the retuning glide once.
    void push_inputs(SampleType left_in, SampleType right_in) noexcept {
        if (!prepared_)
            return;
        (void)right_going_.push(static_cast<double>(left_in));
        (void)left_going_.push(static_cast<double>(right_in));
        (void)length_.next();
    }

    /// Convenience step for feed-forward use: read, then push.
    ///
    /// Invalid input is injected as silence by `FractionalDelayHistoryT`, so a
    /// bad sample cannot poison either rail. Outputs fail closed to silence if
    /// the line is unprepared or a read fails.
    void process(SampleType left_in, SampleType right_in, SampleType& left_out,
                 SampleType& right_out) noexcept {
        read_outputs(left_out, right_out);
        push_inputs(left_in, right_in);
    }

    void reset() noexcept {
        right_going_.reset();
        left_going_.reset();
        length_.set_immediate(target_length_samples_);
    }

    [[nodiscard]] std::size_t retained_bytes() const noexcept {
        return right_going_.retained_bytes() + left_going_.retained_bytes();
    }

  private:
    [[nodiscard]] static SampleType finite_cast(double value) noexcept {
        const auto limit = static_cast<double>(std::numeric_limits<SampleType>::max());
        if (!std::isfinite(value) || value > limit || value < -limit)
            return SampleType{};
        return static_cast<SampleType>(value);
    }

    FractionalDelayHistoryT<double> right_going_{};
    FractionalDelayHistoryT<double> left_going_{};
    SmoothedValue<double> length_{minimum_length_samples};
    double sample_rate_ = 0.0;
    double maximum_length_samples_ = 0.0;
    double target_length_samples_ = minimum_length_samples;
    bool length_configured_ = false;
    bool prepared_ = false;
};

using WaveguideLine = WaveguideLineT<float>;
using WaveguideLine64 = WaveguideLineT<double>;

} // namespace pulp::signal
