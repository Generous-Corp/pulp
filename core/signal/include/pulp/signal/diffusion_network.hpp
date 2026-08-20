#pragma once

/// @file diffusion_network.hpp
/// Fixed-capacity, wet-only true-stereo Schroeder diffusion network.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/detail/schroeder_allpass.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

/// A bounded cascade of stereo Schroeder allpasses with energy-preserving
/// inter-channel coupling. It smears transients but contains no feedback tank,
/// decay control, damping, modulation, dry path, or reverb policy.
///
/// Each scalar allpass is stable for `abs(gain) < 1` and has unit magnitude.
/// Each stage then applies an orthonormal stereo rotation. Consequently, the
/// complete network preserves infinite-horizon L2 energy (apart from floating-
/// point roundoff and the shared below-1e-15 tail flush) and cannot create
/// energy through its width control.
///
/// `prepare()` allocates and belongs on the control thread. `configure()` is a
/// bounded, allocation-free control-thread operation. `process_sample()`,
/// `process_block()`, and `reset()` are bounded, lock-free, and allocation-free.
/// The scalar recurrence is shared with the FDN and CharacterDelay diffusers;
/// this class retains its own contiguous rings because exact one-sample capacity
/// and constant-time logical reset differ from their fractional-delay storage.
template <typename SampleType = float> class DiffusionNetworkT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    static constexpr std::size_t kMaxStages = 8;
    static constexpr double kMinimumSampleRate = 1000.0;
    static constexpr double kMaximumSampleRate = 768000.0;
    static constexpr double kMaximumDelayMs = 100.0;
    // Across all eight stages, 0.02^8 / sqrt(2) remains above the shared
    // 1e-15 snap threshold. Therefore every accepted non-zero coefficient
    // contributes observable unit-impulse feed-through on at least one output.
    static constexpr double kMinimumNonzeroAllpassGain = 0.02;
    static constexpr double kMaximumAllpassGain = 0.95;
    // sin(width*pi/4)^8 remains above the shared 1e-15 snap threshold, so
    // latency-defining routes remain observable through the deepest cascade.
    static constexpr double kMinimumNonzeroStereoWidth = 0.02;

    struct StageConfig {
        double delay_ms = 3.1;
        double gain = 0.65;
    };

    struct Config {
        std::size_t stage_count = 4;
        std::array<StageConfig, kMaxStages> stages{{
            {3.1, 0.65},
            {4.7, 0.62},
            {6.9, 0.59},
            {10.1, 0.56},
            {13.7, 0.53},
            {17.3, 0.50},
            {22.1, 0.47},
            {29.9, 0.44},
        }};
        /// Zero leaves two decorrelated mono cascades. One applies a 45-degree
        /// orthonormal rotation at every stage; intermediate values interpolate
        /// the rotation angle without changing gain.
        double stereo_width = 0.75;
    };

    [[nodiscard]] bool prepare(double sample_rate, double maximum_delay_ms = 50.0) {
        if (!valid_prepare_arguments(sample_rate, maximum_delay_ms))
            return false;

        const double required = std::ceil(sample_rate * maximum_delay_ms / 1000.0) + 1.0;
        if (required >
                static_cast<double>(std::numeric_limits<std::size_t>::max() / (2u * kMaxStages)) ||
            required > static_cast<double>(std::numeric_limits<int>::max()))
            return false;
        const auto capacity = static_cast<std::size_t>(required);

        DerivedConfig replacement_derived;
        if (!derive_config(config_, sample_rate, maximum_delay_ms, capacity, replacement_derived))
            return false;

        std::vector<SampleType> replacement;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            replacement.assign(2u * kMaxStages * capacity, SampleType{});
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
#endif

        storage_ = std::move(replacement);
        sample_rate_ = sample_rate;
        maximum_delay_ms_ = maximum_delay_ms;
        capacity_ = capacity;
        derived_ = replacement_derived;
        prepared_ = true;
        reset();
        derived_.latency_samples = calculate_latency_samples();
        reset();
        return true;
    }

    [[nodiscard]] bool configure(const Config& candidate) noexcept {
        if (!valid_config_shape(candidate))
            return false;
        if (!prepared_) {
            config_ = candidate;
            return true;
        }
        DerivedConfig replacement;
        if (!derive_config(candidate, sample_rate_, maximum_delay_ms_, capacity_, replacement))
            return false;
        config_ = candidate;
        derived_ = replacement;
        reset();
        derived_.latency_samples = calculate_latency_samples();
        reset();
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
    const Config& config() const noexcept {
        return config_;
    }

    int stage_delay_samples(std::size_t stage, std::size_t channel) const noexcept {
        if (stage >= config_.stage_count || channel >= 2u)
            return 0;
        return channel == 0u ? derived_.left_delay[stage] : derived_.right_delay[stage];
    }

    std::size_t retained_bytes() const noexcept {
        return storage_.size() * sizeof(SampleType);
    }

    /// Earliest non-zero response. A non-zero allpass coefficient has direct
    /// feed-through; a zero coefficient degenerates to a pure delay.
    int latency_samples() const noexcept {
        return prepared_ ? derived_.latency_samples : 0;
    }

    /// `-1` denotes the mathematical infinite tail of any recursive allpass.
    /// A zero-gain cascade is feed-forward and reports a conservative latest
    /// impulse-sample bound instead.
    int tail_samples() const noexcept {
        if (!prepared_ || config_.stage_count == 0u)
            return 0;
        int finite_tail = 0;
        for (std::size_t i = 0; i < config_.stage_count; ++i) {
            if (config_.stages[i].gain != 0.0)
                return -1;
            finite_tail += std::max(derived_.left_delay[i], derived_.right_delay[i]);
        }
        return finite_tail;
    }

    void reset() noexcept {
        for (auto& positions : write_positions_)
            positions = {0u, 0u};
        for (auto& valid : valid_samples_)
            valid = {0u, 0u};
    }

    void process_sample(SampleType input_left, SampleType input_right, SampleType& output_left,
                        SampleType& output_right) noexcept {
        output_left = SampleType{};
        output_right = SampleType{};
        if (!prepared_)
            return;
        if (!finite(input_left) || !finite(input_right)) {
            reset();
            return;
        }

        SampleType left = input_left;
        SampleType right = input_right;
        for (std::size_t stage = 0; stage < config_.stage_count; ++stage) {
            if (!process_stage(stage, left, right)) {
                reset();
                return;
            }
        }
        output_left = snap_to_zero(left);
        output_right = snap_to_zero(right);
    }

    void process_block(const SampleType* input_left, const SampleType* input_right,
                       SampleType* output_left, SampleType* output_right,
                       std::size_t sample_count) noexcept {
        if (input_left == nullptr || input_right == nullptr || output_left == nullptr ||
            output_right == nullptr)
            return;
        for (std::size_t i = 0; i < sample_count; ++i) {
            const SampleType left = input_left[i];
            const SampleType right = input_right[i];
            process_sample(left, right, output_left[i], output_right[i]);
        }
    }

  private:
    struct DerivedConfig {
        std::array<int, kMaxStages> left_delay{};
        std::array<int, kMaxStages> right_delay{};
        SampleType rotation_cos = SampleType{1};
        SampleType rotation_sin = SampleType{};
        int latency_samples = 0;
    };

    static constexpr std::array<int, kMaxStages> kRightOffsets{{3, 5, 7, 11, 13, 17, 19, 23}};

    static bool valid_prepare_arguments(double sample_rate, double maximum_delay_ms) noexcept {
        return std::isfinite(sample_rate) && sample_rate >= kMinimumSampleRate &&
               sample_rate <= kMaximumSampleRate && std::isfinite(maximum_delay_ms) &&
               maximum_delay_ms >= 1000.0 / sample_rate && maximum_delay_ms <= kMaximumDelayMs;
    }

    static bool valid_config_shape(const Config& candidate) noexcept {
        if (candidate.stage_count > kMaxStages || !std::isfinite(candidate.stereo_width) ||
            candidate.stereo_width < 0.0 || candidate.stereo_width > 1.0 ||
            (candidate.stereo_width != 0.0 &&
             candidate.stereo_width < kMinimumNonzeroStereoWidth))
            return false;
        for (std::size_t i = 0; i < candidate.stage_count; ++i) {
            const auto& stage = candidate.stages[i];
            const double magnitude = std::abs(stage.gain);
            if (!std::isfinite(stage.delay_ms) || stage.delay_ms <= 0.0 ||
                stage.delay_ms > kMaximumDelayMs || !std::isfinite(stage.gain) ||
                magnitude > kMaximumAllpassGain ||
                (magnitude != 0.0 && magnitude < kMinimumNonzeroAllpassGain))
                return false;
        }
        return true;
    }

    static bool derive_config(const Config& candidate, double sample_rate, double maximum_delay_ms,
                              std::size_t capacity, DerivedConfig& result) noexcept {
        if (!valid_config_shape(candidate))
            return false;
        const int maximum_delay = static_cast<int>(capacity - 1u);
        for (std::size_t i = 0; i < candidate.stage_count; ++i) {
            const auto& stage = candidate.stages[i];
            if (!std::isfinite(stage.delay_ms) || stage.delay_ms < 1000.0 / sample_rate ||
                stage.delay_ms > maximum_delay_ms)
                return false;
            const int left =
                std::clamp(static_cast<int>(std::llround(stage.delay_ms * sample_rate / 1000.0)), 1,
                           maximum_delay);
            const int offset = kRightOffsets[i];
            int right = left;
            if (left + offset <= maximum_delay)
                right = left + offset;
            else if (left - offset >= 1)
                right = left - offset;
            result.left_delay[i] = left;
            result.right_delay[i] = right;
        }
        const double angle = candidate.stereo_width * std::numbers::pi / 4.0;
        result.rotation_cos = static_cast<SampleType>(std::cos(angle));
        result.rotation_sin = static_cast<SampleType>(std::sin(angle));
        return finite(result.rotation_cos) && finite(result.rotation_sin);
    }

    static bool finite(SampleType value) noexcept {
        return std::isfinite(static_cast<double>(value));
    }

    int calculate_latency_samples() noexcept {
        int direct_path_horizon = 0;
        for (std::size_t stage = 0; stage < config_.stage_count; ++stage) {
            if (config_.stages[stage].gain == 0.0) {
                direct_path_horizon +=
                    std::max(derived_.left_delay[stage], derived_.right_delay[stage]);
            }
        }

        int earliest = std::numeric_limits<int>::max();
        for (std::size_t input_channel = 0; input_channel < 2u; ++input_channel) {
            reset();
            for (int sample = 0; sample <= direct_path_horizon; ++sample) {
                SampleType left{};
                SampleType right{};
                const SampleType impulse = sample == 0 ? SampleType{1} : SampleType{};
                process_sample(input_channel == 0u ? impulse : SampleType{},
                               input_channel == 1u ? impulse : SampleType{}, left, right);
                if (left != SampleType{} || right != SampleType{}) {
                    earliest = std::min(earliest, sample);
                    break;
                }
            }
            if (earliest == 0)
                break;
        }
        return earliest == std::numeric_limits<int>::max() ? 0 : earliest;
    }

    std::size_t base_index(std::size_t stage, std::size_t channel) const noexcept {
        return (2u * stage + channel) * capacity_;
    }

    SampleType delayed(std::size_t stage, std::size_t channel, int delay) const noexcept {
        if (static_cast<std::size_t>(delay) > valid_samples_[stage][channel])
            return SampleType{};
        const auto position = write_positions_[stage][channel];
        const auto read_position =
            (position + capacity_ - static_cast<std::size_t>(delay)) % capacity_;
        return storage_[base_index(stage, channel) + read_position];
    }

    bool process_stage(std::size_t stage, SampleType& left, SampleType& right) noexcept {
        const SampleType delayed_left = delayed(stage, 0u, derived_.left_delay[stage]);
        const SampleType delayed_right = delayed(stage, 1u, derived_.right_delay[stage]);
        const long double gain = static_cast<long double>(config_.stages[stage].gain);
        const long double write_left = detail::schroeder_allpass_write(
            static_cast<long double>(left), static_cast<long double>(delayed_left), gain);
        const long double write_right = detail::schroeder_allpass_write(
            static_cast<long double>(right), static_cast<long double>(delayed_right), gain);
        const long double allpass_left =
            detail::schroeder_allpass_output(static_cast<long double>(delayed_left), write_left,
                                             gain);
        const long double allpass_right =
            detail::schroeder_allpass_output(static_cast<long double>(delayed_right), write_right,
                                             gain);
        const long double limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
        if (!std::isfinite(write_left) || !std::isfinite(write_right) ||
            !std::isfinite(allpass_left) || !std::isfinite(allpass_right) ||
            std::abs(write_left) > limit || std::abs(write_right) > limit ||
            std::abs(allpass_left) > limit || std::abs(allpass_right) > limit)
            return false;

        const auto left_position = write_positions_[stage][0];
        const auto right_position = write_positions_[stage][1];
        storage_[base_index(stage, 0u) + left_position] =
            snap_to_zero(static_cast<SampleType>(write_left));
        storage_[base_index(stage, 1u) + right_position] =
            snap_to_zero(static_cast<SampleType>(write_right));
        write_positions_[stage][0] = (left_position + 1u) % capacity_;
        write_positions_[stage][1] = (right_position + 1u) % capacity_;
        valid_samples_[stage][0] = std::min(valid_samples_[stage][0] + 1u, capacity_);
        valid_samples_[stage][1] = std::min(valid_samples_[stage][1] + 1u, capacity_);

        const long double c = static_cast<long double>(derived_.rotation_cos);
        const long double s = static_cast<long double>(derived_.rotation_sin);
        const long double sign = (stage & 1u) == 0u ? 1.0L : -1.0L;
        const long double rotated_left = c * allpass_left - sign * s * allpass_right;
        const long double rotated_right = sign * s * allpass_left + c * allpass_right;
        if (!std::isfinite(rotated_left) || !std::isfinite(rotated_right) ||
            std::abs(rotated_left) > limit || std::abs(rotated_right) > limit)
            return false;
        left = static_cast<SampleType>(rotated_left);
        right = static_cast<SampleType>(rotated_right);
        return true;
    }

    Config config_{};
    DerivedConfig derived_{};
    std::vector<SampleType> storage_{};
    std::array<std::array<std::size_t, 2>, kMaxStages> write_positions_{};
    std::array<std::array<std::size_t, 2>, kMaxStages> valid_samples_{};
    double sample_rate_ = 48000.0;
    double maximum_delay_ms_ = 50.0;
    std::size_t capacity_ = 0u;
    bool prepared_ = false;
};

using DiffusionNetwork = DiffusionNetworkT<float>;
using DiffusionNetwork64 = DiffusionNetworkT<double>;

} // namespace pulp::signal
