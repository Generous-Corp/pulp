#pragma once

/// @file early_reflections.hpp
/// Fixed-capacity, caller-authored true-stereo early-reflection taps.

#include <pulp/signal/audio_matrix_mixer.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/panner.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pulp::signal {

/// A wet-only early-reflection renderer with a caller-authored tap pattern.
///
/// This is the reusable rendering primitive, not a room or image-source model:
/// callers own the geometry (if any) and publish a complete bounded tap set.
/// `prepare()` and `configure()` are control-thread operations. Processing and
/// constant-time reset allocate no memory, take no locks, and preserve stereo
/// input identity when a tap uses the default pan/width.
template <typename SampleType = float> class EarlyReflectionsT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    static constexpr std::size_t kMaxTaps = 16;
    static constexpr double kMinimumSampleRate = 1000.0;
    static constexpr double kMaximumSampleRate = 768000.0;
    static constexpr double kMaximumDelayMs = 1000.0;

    struct Tap {
        double delay_ms = 20.0;
        double gain = 1.0;
        /// Centre of the tap's stereo image, in [-1, 1].
        double pan = 0.0;
        /// Separation of its left/right sources, in [0, 1]. Zero collapses
        /// both sources to `pan`; one at centre preserves hard L/R identity.
        double stereo_width = 1.0;
    };

    [[nodiscard]] bool prepare(double sample_rate, double maximum_delay_ms = 250.0) {
        if (!valid_prepare_request(sample_rate, maximum_delay_ms) ||
            !taps_fit(taps_, tap_count_, sample_rate, maximum_delay_ms))
            return false;

        const double required = std::ceil(sample_rate * maximum_delay_ms / 1000.0) + 1.0;
        if (required > static_cast<double>(std::numeric_limits<int>::max() - 1))
            return false;

        DelayLineT<SampleType> replacement_left;
        DelayLineT<SampleType> replacement_right;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            replacement_left.prepare(static_cast<int>(required));
            replacement_right.prepare(static_cast<int>(required));
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
#endif

        auto replacement_derived = derive_taps(taps_, tap_count_, sample_rate);
        apply_headroom(replacement_derived, tap_count_, headroom_policy_);
        left_history_ = std::move(replacement_left);
        right_history_ = std::move(replacement_right);
        derived_ = replacement_derived;
        sample_rate_ = sample_rate;
        maximum_delay_ms_ = maximum_delay_ms;
        prepared_ = true;
        reset();
        return true;
    }

    /// Atomically replaces the complete tap pattern. A rejected pattern leaves
    /// the active taps, routing gains, headroom policy, and history unchanged.
    [[nodiscard]] bool
    configure(std::span<const Tap> taps,
              MatrixHeadroomPolicy policy = MatrixHeadroomPolicy::NormalizePeak) noexcept {
        if (taps.size() > kMaxTaps)
            return false;

        std::array<Tap, kMaxTaps> replacement{};
        for (std::size_t i = 0; i < taps.size(); ++i) {
            if (!valid_tap(taps[i]))
                return false;
            replacement[i] = taps[i];
        }
        if (prepared_ && !taps_fit(replacement, taps.size(), sample_rate_, maximum_delay_ms_))
            return false;

        auto replacement_derived = derive_taps(replacement, taps.size(), sample_rate_);
        apply_headroom(replacement_derived, taps.size(), policy);
        taps_ = replacement;
        derived_ = replacement_derived;
        tap_count_ = taps.size();
        headroom_policy_ = policy;
        return true;
    }

    bool prepared() const noexcept {
        return prepared_;
    }
    double sample_rate() const noexcept {
        return sample_rate_;
    }
    double maximum_delay_ms() const noexcept {
        return maximum_delay_ms_;
    }
    std::size_t tap_count() const noexcept {
        return tap_count_;
    }
    Tap tap(std::size_t index) const noexcept {
        return index < tap_count_ ? taps_[index] : Tap{};
    }
    MatrixHeadroomPolicy headroom_policy() const noexcept {
        return headroom_policy_;
    }
    std::size_t retained_bytes() const noexcept {
        if (!prepared_)
            return 0;
        const auto samples = static_cast<std::size_t>(left_history_.max_delay() + 1);
        return 2u * samples * sizeof(SampleType);
    }

    /// This wet-only feed-forward renderer never delays a direct path.
    static constexpr int latency_samples() noexcept {
        return 0;
    }

    /// The last possibly non-zero linearly interpolated tap sample.
    int tail_samples() const noexcept {
        if (!prepared_)
            return 0;
        double latest = 0.0;
        for (std::size_t i = 0; i < tap_count_; ++i) {
            const auto& route = derived_[i];
            if (route.left_to_left != 0.0 || route.left_to_right != 0.0 ||
                route.right_to_left != 0.0 || route.right_to_right != 0.0)
                latest = std::max(latest, route.delay_samples);
        }
        return static_cast<int>(std::ceil(latest));
    }

    /// Logically clears both histories in constant time.
    void reset() noexcept {
        left_history_.discard_history();
        right_history_.discard_history();
    }

    void process_sample(SampleType input_left, SampleType input_right, SampleType& output_left,
                        SampleType& output_right) noexcept {
        output_left = SampleType{};
        output_right = SampleType{};
        if (!prepared_)
            return;
        if (!std::isfinite(static_cast<double>(input_left)) ||
            !std::isfinite(static_cast<double>(input_right))) {
            reset();
            return;
        }

        long double wet_left = 0.0L;
        long double wet_right = 0.0L;
        for (std::size_t i = 0; i < tap_count_; ++i) {
            const auto& route = derived_[i];
            const auto history_delay = static_cast<SampleType>(route.delay_samples - 1.0);
            const auto delayed_left = left_history_.read(history_delay);
            const auto delayed_right = right_history_.read(history_delay);
            wet_left += static_cast<long double>(route.left_to_left) * delayed_left +
                        static_cast<long double>(route.right_to_left) * delayed_right;
            wet_right += static_cast<long double>(route.left_to_right) * delayed_left +
                         static_cast<long double>(route.right_to_right) * delayed_right;
        }

        const auto limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
        if (!std::isfinite(wet_left) || !std::isfinite(wet_right) || std::abs(wet_left) > limit ||
            std::abs(wet_right) > limit) {
            reset();
            return;
        }

        output_left = snap_to_zero(static_cast<SampleType>(wet_left));
        output_right = snap_to_zero(static_cast<SampleType>(wet_right));
        left_history_.push(input_left);
        right_history_.push(input_right);
    }

    void process_block(const SampleType* input_left, const SampleType* input_right,
                       SampleType* output_left, SampleType* output_right,
                       std::size_t sample_count) noexcept {
        if (input_left == nullptr || input_right == nullptr || output_left == nullptr ||
            output_right == nullptr)
            return;
        for (std::size_t i = 0; i < sample_count; ++i) {
            const auto left = input_left[i];
            const auto right = input_right[i];
            process_sample(left, right, output_left[i], output_right[i]);
        }
    }

  private:
    struct DerivedTap {
        double delay_samples = 1.0;
        double left_to_left = 0.0;
        double left_to_right = 0.0;
        double right_to_left = 0.0;
        double right_to_right = 0.0;
    };

    static bool valid_prepare_request(double sample_rate, double maximum_delay_ms) noexcept {
        return std::isfinite(sample_rate) && sample_rate >= kMinimumSampleRate &&
               sample_rate <= kMaximumSampleRate && std::isfinite(maximum_delay_ms) &&
               maximum_delay_ms >= 1000.0 / sample_rate && maximum_delay_ms <= kMaximumDelayMs;
    }

    static bool valid_tap(const Tap& tap) noexcept {
        return std::isfinite(tap.delay_ms) && tap.delay_ms > 0.0 &&
               tap.delay_ms <= kMaximumDelayMs && std::isfinite(tap.gain) &&
               std::abs(tap.gain) <= 1.0 && std::isfinite(tap.pan) && tap.pan >= -1.0 &&
               tap.pan <= 1.0 && std::isfinite(tap.stereo_width) && tap.stereo_width >= 0.0 &&
               tap.stereo_width <= 1.0;
    }

    static bool taps_fit(const std::array<Tap, kMaxTaps>& taps, std::size_t count,
                         double sample_rate, double maximum_delay_ms) noexcept {
        const double minimum_delay_ms = 1000.0 / sample_rate;
        for (std::size_t i = 0; i < count; ++i)
            if (!valid_tap(taps[i]) || taps[i].delay_ms < minimum_delay_ms ||
                taps[i].delay_ms > maximum_delay_ms)
                return false;
        return true;
    }

    static double canonical_delay_samples(double delay_ms, double sample_rate) noexcept {
        const double reconstructed = delay_ms * sample_rate / 1000.0;
        const double nearest_integer = std::round(reconstructed);
        const double tolerance =
            8.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(reconstructed));
        return std::abs(reconstructed - nearest_integer) <= tolerance ? nearest_integer
                                                                      : reconstructed;
    }

    static std::array<DerivedTap, kMaxTaps> derive_taps(const std::array<Tap, kMaxTaps>& taps,
                                                        std::size_t count,
                                                        double sample_rate) noexcept {
        std::array<DerivedTap, kMaxTaps> result{};
        for (std::size_t i = 0; i < count; ++i) {
            const auto& tap = taps[i];
            auto& route = result[i];
            route.delay_samples = canonical_delay_samples(tap.delay_ms, sample_rate);

            const double left_position = std::clamp(tap.pan - tap.stereo_width, -1.0, 1.0);
            const double right_position = std::clamp(tap.pan + tap.stereo_width, -1.0, 1.0);
            PannerT<double> left_panner;
            PannerT<double> right_panner;
            left_panner.set_pan(left_position);
            right_panner.set_pan(right_position);
            auto left = left_panner.process(tap.gain);
            auto right = right_panner.process(tap.gain);
            // Preserve exact channel identity at the endpoints even if a
            // trigonometric panner implementation has endpoint residue.
            if (left_position == -1.0)
                left = {tap.gain, 0.0};
            else if (left_position == 1.0)
                left = {0.0, tap.gain};
            if (right_position == -1.0)
                right = {tap.gain, 0.0};
            else if (right_position == 1.0)
                right = {0.0, tap.gain};
            route.left_to_left = left.left;
            route.left_to_right = left.right;
            route.right_to_left = right.left;
            route.right_to_right = right.right;
        }
        return result;
    }

    static void apply_headroom(std::array<DerivedTap, kMaxTaps>& taps, std::size_t count,
                               MatrixHeadroomPolicy policy) noexcept {
        if (policy == MatrixHeadroomPolicy::Raw)
            return;
        long double left_sum = 0.0L;
        long double right_sum = 0.0L;
        for (std::size_t i = 0; i < count; ++i) {
            left_sum += std::abs(taps[i].left_to_left) + std::abs(taps[i].right_to_left);
            right_sum += std::abs(taps[i].left_to_right) + std::abs(taps[i].right_to_right);
        }
        const double left_scale = static_cast<double>(1.0L / std::max(1.0L, left_sum));
        const double right_scale = static_cast<double>(1.0L / std::max(1.0L, right_sum));
        for (std::size_t i = 0; i < count; ++i) {
            taps[i].left_to_left *= left_scale;
            taps[i].right_to_left *= left_scale;
            taps[i].left_to_right *= right_scale;
            taps[i].right_to_right *= right_scale;
        }
    }

    double sample_rate_ = 48000.0;
    double maximum_delay_ms_ = 250.0;
    std::size_t tap_count_ = 0;
    MatrixHeadroomPolicy headroom_policy_ = MatrixHeadroomPolicy::NormalizePeak;
    bool prepared_ = false;
    std::array<Tap, kMaxTaps> taps_{};
    std::array<DerivedTap, kMaxTaps> derived_{};
    DelayLineT<SampleType> left_history_{};
    DelayLineT<SampleType> right_history_{};
};

using EarlyReflections = EarlyReflectionsT<float>;
using EarlyReflections64 = EarlyReflectionsT<double>;

} // namespace pulp::signal
