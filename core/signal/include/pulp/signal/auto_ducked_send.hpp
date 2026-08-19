#pragma once

/// @file auto_ducked_send.hpp
/// Wet/send-only gain control driven by a source or sidechain detector.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dynamics_contract.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace pulp::signal {

enum class AutoDuckedSendStatus { ready, invalid_sample_rate, invalid_config };

/// Stereo send-gain processor for keeping an effect return out of the way of a
/// source or sidechain signal.
///
/// The detector envelope is converted to a 1:1 attenuation above threshold,
/// capped by `range_db`. `send_gain_db` is then applied independently of that
/// attenuation. This object processes the wet/send input only: it neither
/// accepts nor mixes a dry signal, and it owns no delay, effect, or routing
/// policy. A caller mixes the returned send with dry audio elsewhere.
///
/// `prepare()` and `configure()` belong at a control-thread retiming point.
/// Once prepared, scalar/block processing, bypass, reset, and inspection are
/// bounded, lock-free, allocation-free, and exception-free. Bypass and the
/// neutral gain setting return every finite send sample bit-exactly while the
/// detector continues to advance.
template <typename SampleType = float> class AutoDuckedSendT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    using DetectorMode = typename EnvelopeFollowerT<SampleType>::Mode;

    struct Config {
        SampleType threshold_db = SampleType{-24};
        SampleType range_db = SampleType{12};
        SampleType attack_ms = SampleType{10};
        SampleType release_ms = SampleType{250};
        SampleType send_gain_db = SampleType{0};
        DetectorMode detector = DetectorMode::peak;
        DynamicsStereoLink stereo_link = DynamicsStereoLink::peak_linked;
    };

    static constexpr SampleType kMinThresholdDb = SampleType{-160};
    static constexpr SampleType kMaxThresholdDb = SampleType{24};
    static constexpr SampleType kMaxRangeDb = SampleType{96};
    static constexpr SampleType kMinSendGainDb = SampleType{-160};
    static constexpr SampleType kMaxSendGainDb = SampleType{24};
    static constexpr SampleType kMaxSampleRate = SampleType{1536000};

    [[nodiscard]] AutoDuckedSendStatus prepare(SampleType sample_rate) noexcept {
        if (!finite(sample_rate) || !(sample_rate > SampleType{0}) || sample_rate > kMaxSampleRate)
            return AutoDuckedSendStatus::invalid_sample_rate;
        sample_rate_ = sample_rate;
        apply_config(config_);
        prepared_ = true;
        reset();
        return AutoDuckedSendStatus::ready;
    }

    /// Atomically adopts a complete valid configuration. Rejection preserves
    /// both the previous configuration and all detector history.
    [[nodiscard]] AutoDuckedSendStatus configure(const Config& next) noexcept {
        if (!valid(next))
            return AutoDuckedSendStatus::invalid_config;
        const bool detector_topology_changed =
            next.detector != config_.detector || next.stereo_link != config_.stereo_link;
        config_ = next;
        if (prepared_) {
            apply_config(config_);
            if (detector_topology_changed)
                reset();
        } else {
            current_gain_db_ = {config_.send_gain_db, config_.send_gain_db};
        }
        return AutoDuckedSendStatus::ready;
    }

    /// Pure memoryless detector-level-to-send-gain transfer function.
    [[nodiscard]] static SampleType gain_computer_db(SampleType detector_level_db,
                                                     const Config& config) noexcept {
        if (!finite(detector_level_db) || !valid(config))
            return SampleType{0};
        const SampleType over = std::max(SampleType{0}, detector_level_db - config.threshold_db);
        return config.send_gain_db - std::min(config.range_db, over);
    }

    /// Process a stereo send and an independent stereo detector sample.
    /// Linked mode derives one control value from max(abs(L), abs(R)); detector
    /// polarity therefore cannot cancel. Independent mode controls each send
    /// channel from the corresponding detector channel.
    [[nodiscard]] std::array<SampleType, 2> process(SampleType send_left, SampleType send_right,
                                                    SampleType detector_left,
                                                    SampleType detector_right) noexcept {
        if (!prepared_ || !finite(send_left) || !finite(send_right)) {
            reset();
            return {SampleType{0}, SampleType{0}};
        }
        if (!finite(detector_left) || !finite(detector_right)) {
            reset();
            // Detector failure cannot override a path whose output does not
            // depend on the detector. Bypass stays bit-exact; zero duck range
            // still applies only the independent base send gain.
            if (bypassed_)
                return {send_left, send_right};
            if (config_.range_db == SampleType{0})
                return apply_current_gain(send_left, send_right);
            return {SampleType{0}, SampleType{0}};
        }

        const auto envelope = detector_.process(detector_left, detector_right);
        std::array<SampleType, 2> output{};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const SampleType detector_db = envelope[channel] > SampleType{0}
                                               ? SampleType{20} * std::log10(envelope[channel])
                                               : kMinThresholdDb;
            current_gain_db_[channel] = gain_computer_db(detector_db, config_);
            duck_reduction_db_[channel] =
                std::max(SampleType{0}, config_.send_gain_db - current_gain_db_[channel]);
            const SampleType input = channel == 0 ? send_left : send_right;
            if (bypassed_ || current_gain_db_[channel] == SampleType{0}) {
                output[channel] = input;
            } else {
                output[channel] = bounded_multiply(input, db_to_linear(current_gain_db_[channel]));
            }
        }
        return output;
    }

    /// Out-of-place or in-place block processing. The output may alias the
    /// matching send input. Detector buffers must not alias either output.
    void process_block(const SampleType* send_left, const SampleType* send_right,
                       const SampleType* detector_left, const SampleType* detector_right,
                       SampleType* output_left, SampleType* output_right,
                       int num_samples) noexcept {
        if (num_samples <= 0 || send_left == nullptr || send_right == nullptr ||
            detector_left == nullptr || detector_right == nullptr || output_left == nullptr ||
            output_right == nullptr)
            return;
        for (int i = 0; i < num_samples; ++i) {
            const SampleType input_left = send_left[i];
            const SampleType input_right = send_right[i];
            const auto output =
                process(input_left, input_right, detector_left[i], detector_right[i]);
            output_left[i] = output[0];
            output_right[i] = output[1];
        }
    }

    /// Convenience in-place block path for the send buffers.
    void process_block(SampleType* send_left, SampleType* send_right,
                       const SampleType* detector_left, const SampleType* detector_right,
                       int num_samples) noexcept {
        process_block(send_left, send_right, detector_left, detector_right, send_left, send_right,
                      num_samples);
    }

    void set_bypassed(bool bypassed) noexcept {
        bypassed_ = bypassed;
    }
    [[nodiscard]] bool bypassed() const noexcept {
        return bypassed_;
    }

    void reset() noexcept {
        detector_.reset();
        current_gain_db_ = {config_.send_gain_db, config_.send_gain_db};
        duck_reduction_db_ = {};
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
    [[nodiscard]] static constexpr int latency_samples() noexcept {
        return 0;
    }
    [[nodiscard]] static constexpr int tail_samples() noexcept {
        return 0;
    }
    [[nodiscard]] std::array<SampleType, 2> detector_envelope() const noexcept {
        return detector_.current();
    }
    [[nodiscard]] std::array<SampleType, 2> current_gain_db() const noexcept {
        return current_gain_db_;
    }
    [[nodiscard]] std::array<GainReduction, 2> gain_reduction() const noexcept {
        return {GainReduction::from_magnitude_db(duck_reduction_db_[0]),
                GainReduction::from_magnitude_db(duck_reduction_db_[1])};
    }

  private:
    static bool finite(SampleType value) noexcept {
        return std::isfinite(static_cast<double>(value));
    }

    static bool valid(const Config& config) noexcept {
        return finite(config.threshold_db) && config.threshold_db >= kMinThresholdDb &&
               config.threshold_db <= kMaxThresholdDb && finite(config.range_db) &&
               config.range_db >= SampleType{0} && config.range_db <= kMaxRangeDb &&
               finite(config.attack_ms) && config.attack_ms >= SampleType{0.01} &&
               config.attack_ms <= SampleType{2000} && finite(config.release_ms) &&
               config.release_ms >= SampleType{0.01} && config.release_ms <= SampleType{10000} &&
               finite(config.send_gain_db) && config.send_gain_db >= kMinSendGainDb &&
               config.send_gain_db <= kMaxSendGainDb &&
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

    std::array<SampleType, 2> apply_current_gain(SampleType left, SampleType right) const noexcept {
        if (config_.send_gain_db == SampleType{0})
            return {left, right};
        const SampleType gain = db_to_linear(config_.send_gain_db);
        return {bounded_multiply(left, gain), bounded_multiply(right, gain)};
    }

    void apply_config(const Config& config) noexcept {
        detector_.prepare(sample_rate_);
        detector_.set_mode(config.detector);
        detector_.set_attack_ms(config.attack_ms);
        detector_.set_release_ms(config.release_ms);
        detector_.set_link(config.stereo_link == DynamicsStereoLink::peak_linked ? SampleType{1}
                                                                                 : SampleType{0});
    }

    Config config_{};
    StereoEnvelopeFollowerT<SampleType> detector_{};
    std::array<SampleType, 2> current_gain_db_{};
    std::array<SampleType, 2> duck_reduction_db_{};
    SampleType sample_rate_ = SampleType{0};
    bool prepared_ = false;
    bool bypassed_ = false;
};

using AutoDuckedSend = AutoDuckedSendT<float>;
using AutoDuckedSend64 = AutoDuckedSendT<double>;

} // namespace pulp::signal
