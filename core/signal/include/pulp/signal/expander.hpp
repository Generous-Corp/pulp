#pragma once

/// @file expander.hpp
/// Bounded upward/downward expansion with shared dynamics telemetry.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dynamics_contract.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace pulp::signal {

enum class ExpansionMode { downward, upward };
enum class ExpanderStatus { ready, invalid_sample_rate, invalid_config };

/// Stereo upward/downward expander with a continuous bounded gain law.
///
/// The prepared peak/RMS follower supplies detector ballistics. `prepare()` and
/// `configure()` belong off the audio thread. After preparation, processing,
/// bypass, reset, and inspection allocate no memory, acquire no locks, perform
/// no I/O, and throw no exceptions. Bypass returns the finite input samples
/// exactly while advancing detector state, so leaving bypass has no stale-state
/// discontinuity.
template <typename SampleType = float> class ExpanderT {
  public:
    using DetectorMode = typename EnvelopeFollowerT<SampleType>::Mode;

    struct Config {
        ExpansionMode mode = ExpansionMode::downward;
        SampleType threshold_db = SampleType{-40};
        SampleType ratio = SampleType{2};
        SampleType range_db = SampleType{24};
        SampleType knee_db = SampleType{6};
        SampleType attack_ms = SampleType{5};
        SampleType release_ms = SampleType{100};
        DetectorMode detector = DetectorMode::rms;
        DynamicsStereoLink stereo_link = DynamicsStereoLink::peak_linked;
    };

    static constexpr SampleType kMinThresholdDb = SampleType{-160};
    static constexpr SampleType kMaxThresholdDb = SampleType{24};
    static constexpr SampleType kMinRatio = SampleType{1};
    static constexpr SampleType kMaxRatio = SampleType{20};
    static constexpr SampleType kMaxRangeDb = SampleType{96};
    static constexpr SampleType kMaxKneeDb = SampleType{48};
    static constexpr SampleType kMaxSampleRate = SampleType{1536000};

    [[nodiscard]] ExpanderStatus prepare(SampleType sample_rate) noexcept {
        if (!finite(sample_rate) || !(sample_rate > SampleType{0}) || sample_rate > kMaxSampleRate)
            return ExpanderStatus::invalid_sample_rate;
        sample_rate_ = sample_rate;
        apply_config(config_);
        prepared_ = true;
        reset();
        return ExpanderStatus::ready;
    }

    /// Adopt a complete valid configuration. Rejected input leaves both the
    /// current configuration and detector history unchanged.
    [[nodiscard]] ExpanderStatus configure(const Config& next) noexcept {
        if (!valid(next))
            return ExpanderStatus::invalid_config;
        const bool detector_topology_changed =
            next.detector != config_.detector || next.stereo_link != config_.stereo_link;
        config_ = next;
        if (prepared_) {
            apply_config(config_);
            if (detector_topology_changed)
                reset();
        }
        return ExpanderStatus::ready;
    }

    /// Pure memoryless curve used by the processor after envelope detection.
    [[nodiscard]] static SampleType gain_computer_db(SampleType level_db,
                                                     const Config& config) noexcept {
        if (!finite(level_db) || !valid(config))
            return SampleType{0};
        const SampleType x = level_db - config.threshold_db;
        const SampleType slope = config.ratio - SampleType{1};
        SampleType gain{};
        if (config.mode == ExpansionMode::downward) {
            if (slope > SampleType{0} && x <= -config.range_db / slope)
                return -config.range_db;
            if (!(config.knee_db > SampleType{0})) {
                gain = x < SampleType{0} ? slope * x : SampleType{0};
            } else {
                const SampleType half = config.knee_db / SampleType{2};
                if (x <= -half) {
                    gain = slope * x;
                } else if (x < half) {
                    const SampleType distance = half - x;
                    gain = -slope * distance * distance / (SampleType{2} * config.knee_db);
                }
            }
            return std::max(-config.range_db, gain);
        }

        if (slope > SampleType{0} && x >= config.range_db / slope)
            return config.range_db;
        if (!(config.knee_db > SampleType{0})) {
            gain = x > SampleType{0} ? slope * x : SampleType{0};
        } else {
            const SampleType half = config.knee_db / SampleType{2};
            if (x >= half) {
                gain = slope * x;
            } else if (x > -half) {
                const SampleType distance = x + half;
                gain = slope * distance * distance / (SampleType{2} * config.knee_db);
            }
        }
        return std::min(config.range_db, gain);
    }

    std::array<SampleType, 2> process(SampleType left, SampleType right) noexcept {
        if (!prepared_ || !finite(left) || !finite(right)) {
            reset();
            return {SampleType{0}, SampleType{0}};
        }

        const auto envelope = process_detector(left, right);
        std::array<SampleType, 2> output{};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            if (envelope[channel] > SampleType{0}) {
                const SampleType level = SampleType{20} * std::log10(envelope[channel]);
                current_gain_db_[channel] = gain_computer_db(level, config_);
            } else {
                current_gain_db_[channel] =
                    config_.mode == ExpansionMode::downward && config_.ratio > SampleType{1}
                        ? -config_.range_db
                        : SampleType{0};
            }
            output[channel] = bounded_multiply(channel == 0 ? left : right,
                                               db_to_linear(current_gain_db_[channel]));
        }
        if (bypassed_)
            return {left, right};
        return output;
    }

    void process(SampleType* left, SampleType* right, int num_samples) noexcept {
        if (num_samples <= 0 || left == nullptr || right == nullptr)
            return;
        for (int i = 0; i < num_samples; ++i) {
            const auto output = process(left[i], right[i]);
            left[i] = output[0];
            right[i] = output[1];
        }
    }

    void set_bypassed(bool bypassed) noexcept {
        bypassed_ = bypassed;
    }
    [[nodiscard]] bool bypassed() const noexcept {
        return bypassed_;
    }

    void reset() noexcept {
        detector_state_ = {};
        current_gain_db_ = {};
    }

    [[nodiscard]] const Config& config() const noexcept {
        return config_;
    }
    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] SampleType sample_rate() const noexcept {
        return sample_rate_;
    }
    [[nodiscard]] int latency_samples() const noexcept {
        return 0;
    }
    [[nodiscard]] int tail_samples() const noexcept {
        return 0;
    }
    [[nodiscard]] std::array<SampleType, 2> current_gain_db() const noexcept {
        return current_gain_db_;
    }
    [[nodiscard]] std::array<GainReduction, 2> gain_reduction() const noexcept {
        return {GainReduction::from_signed_db(current_gain_db_[0]),
                GainReduction::from_signed_db(current_gain_db_[1])};
    }

  private:
    static bool finite(SampleType value) noexcept {
        return std::isfinite(static_cast<double>(value));
    }

    static bool valid(const Config& config) noexcept {
        return (config.mode == ExpansionMode::downward || config.mode == ExpansionMode::upward) &&
               finite(config.threshold_db) && config.threshold_db >= kMinThresholdDb &&
               config.threshold_db <= kMaxThresholdDb && finite(config.ratio) &&
               config.ratio >= kMinRatio && config.ratio <= kMaxRatio && finite(config.range_db) &&
               config.range_db >= SampleType{0} && config.range_db <= kMaxRangeDb &&
               finite(config.knee_db) && config.knee_db >= SampleType{0} &&
               config.knee_db <= kMaxKneeDb && finite(config.attack_ms) &&
               config.attack_ms >= SampleType{0.01} && config.attack_ms <= SampleType{2000} &&
               finite(config.release_ms) && config.release_ms >= SampleType{0.01} &&
               config.release_ms <= SampleType{10000} &&
               (config.detector == DetectorMode::peak || config.detector == DetectorMode::rms) &&
               (config.stereo_link == DynamicsStereoLink::independent ||
                config.stereo_link == DynamicsStereoLink::peak_linked);
    }

    static SampleType db_to_linear(SampleType db) noexcept {
        return std::pow(SampleType{10}, db / SampleType{20});
    }

    static SampleType bounded_multiply(SampleType input, SampleType gain) noexcept {
        const SampleType limit = std::numeric_limits<SampleType>::max();
        if (gain > SampleType{1} && std::abs(input) > limit / gain)
            return std::copysign(limit, input);
        return snap_to_zero(input * gain);
    }

    void apply_config(const Config& config) noexcept {
        attack_coefficient_ =
            EnvelopeFollowerT<SampleType>::coefficient_for_time_ms(config.attack_ms, sample_rate_);
        release_coefficient_ =
            EnvelopeFollowerT<SampleType>::coefficient_for_time_ms(config.release_ms, sample_rate_);
    }

    std::array<SampleType, 2> process_detector(SampleType left, SampleType right) noexcept {
        std::array<SampleType, 2> magnitudes{std::abs(left), std::abs(right)};
        if (config_.stereo_link == DynamicsStereoLink::peak_linked) {
            const SampleType shared = std::max(magnitudes[0], magnitudes[1]);
            magnitudes = {shared, shared};
        }

        std::array<SampleType, 2> envelope{};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            SampleType detector_value = magnitudes[channel];
            if (config_.detector == DetectorMode::rms) {
                const SampleType square_limit = std::sqrt(std::numeric_limits<SampleType>::max());
                detector_value = magnitudes[channel] > square_limit
                                     ? std::numeric_limits<SampleType>::max()
                                     : magnitudes[channel] * magnitudes[channel];
            }
            const SampleType coefficient = detector_value > detector_state_[channel]
                                               ? attack_coefficient_
                                               : release_coefficient_;
            detector_state_[channel] += coefficient * (detector_value - detector_state_[channel]);
            if (!finite(detector_state_[channel]) || detector_state_[channel] < SampleType{0}) {
                reset();
                return {};
            }
            if (detector_state_[channel] < std::numeric_limits<SampleType>::min())
                detector_state_[channel] = SampleType{0};
            envelope[channel] = config_.detector == DetectorMode::rms
                                    ? std::sqrt(detector_state_[channel])
                                    : detector_state_[channel];
        }
        return envelope;
    }

    Config config_{};
    std::array<SampleType, 2> detector_state_{};
    std::array<SampleType, 2> current_gain_db_{};
    SampleType attack_coefficient_ = SampleType{1};
    SampleType release_coefficient_ = SampleType{1};
    SampleType sample_rate_ = SampleType{0};
    bool prepared_ = false;
    bool bypassed_ = false;
};

using Expander = ExpanderT<float>;
using Expander64 = ExpanderT<double>;

} // namespace pulp::signal
