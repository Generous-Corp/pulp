#pragma once

/// @file scope_capture.hpp
/// Fixed-capacity streaming oscilloscope trigger and capture.
///
/// Capture windows contain `pretrigger_samples` samples before the trigger,
/// followed by the trigger sample and the configured post-trigger tail. Edge
/// modes use a level plus hysteresis and can run once or continuously with an
/// exact sample-count holdoff. Continuous acquisition keeps the most recently
/// completed frame published while the next frame is in progress. Processing
/// is allocation-free, lock-free, and independent of caller block partitioning.
/// `capture_completion_latency_samples()` is acquisition readiness after the
/// trigger sample, not signal-processing or host-compensation latency.
/// `capture()` exposes same-thread state only; callers must provide their own
/// synchronized snapshot/mailbox when publishing frames across threads.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class ScopeTriggerMode : std::uint8_t {
    immediate,
    rising_edge,
    falling_edge,
    either_edge,
};

struct ScopeCaptureConfig {
    std::size_t capture_samples = 1024;
    std::size_t pretrigger_samples = 256;
    ScopeTriggerMode trigger_mode = ScopeTriggerMode::rising_edge;
    double trigger_level = 0.0;
    double hysteresis = 0.0;
    std::uint64_t holdoff_samples = 0;
    bool continuous = false;
};

template <typename SampleType, std::size_t MaxSamples> struct ScopeCaptureFrameT {
    static_assert(std::is_floating_point_v<SampleType>);

    std::array<SampleType, MaxSamples> samples{};
    std::size_t sample_count = 0;
    std::size_t trigger_offset = 0;
    std::uint64_t trigger_sample_index = 0;
    std::uint64_t generation = 0;
    std::uint64_t nonfinite_samples_seen = 0;
};

template <typename SampleType = float, std::size_t MaxSamples = 8192> class ScopeCaptureT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxSamples > 0);

  public:
    using Frame = ScopeCaptureFrameT<SampleType, MaxSamples>;

    ScopeCaptureT() noexcept {
        ScopeCaptureConfig config;
        config.capture_samples = std::min<std::size_t>(1024, MaxSamples);
        config.pretrigger_samples = std::min<std::size_t>(256, config.capture_samples - 1);
        (void)configure(config);
    }

    /// Control-thread operation. Invalid configurations leave all state intact.
    [[nodiscard]] bool configure(const ScopeCaptureConfig& config) noexcept {
        const double low = config.trigger_level - config.hysteresis;
        const double high = config.trigger_level + config.hysteresis;
        const auto mode_valid = config.trigger_mode == ScopeTriggerMode::immediate ||
                                config.trigger_mode == ScopeTriggerMode::rising_edge ||
                                config.trigger_mode == ScopeTriggerMode::falling_edge ||
                                config.trigger_mode == ScopeTriggerMode::either_edge;
        if (config.capture_samples == 0 || config.capture_samples > MaxSamples ||
            config.pretrigger_samples >= config.capture_samples ||
            !std::isfinite(config.trigger_level) || !std::isfinite(config.hysteresis) ||
            config.hysteresis < 0.0 || !std::isfinite(low) || !std::isfinite(high) ||
            !representable_(low) || !representable_(high) || !mode_valid) {
            return false;
        }
        config_ = config;
        reset();
        return true;
    }

    void reset() noexcept {
        workspace_.fill(SampleType{0});
        frame_ = {};
        state_ = State::armed;
        history_write_ = 0;
        history_count_ = 0;
        capture_write_ = 0;
        holdoff_remaining_ = 0;
        samples_seen_ = 0;
        generation_ = 0;
        nonfinite_samples_seen_ = 0;
        capture_trigger_sample_index_ = 0;
        rising_primed_ = false;
        falling_primed_ = false;
        ready_ = false;
    }

    /// Arms a one-shot capture without discarding accumulated pretrigger history.
    void arm() noexcept {
        if (state_ == State::capturing) {
            history_count_ = capture_write_;
            history_write_ = capture_write_ % MaxSamples;
        }
        state_ = State::armed;
        capture_write_ = 0;
        holdoff_remaining_ = 0;
        rising_primed_ = false;
        falling_primed_ = false;
        ready_ = false;
    }

    void process(std::span<const SampleType> input) noexcept {
        for (const auto sample : input)
            process_sample_(sample);
    }

    [[nodiscard]] bool capture_ready() const noexcept {
        return ready_;
    }
    [[nodiscard]] bool armed() const noexcept {
        return state_ == State::armed;
    }
    [[nodiscard]] bool capturing() const noexcept {
        return state_ == State::capturing;
    }
    /// Same-thread view. This reference is not a concurrent publication API.
    [[nodiscard]] const Frame& capture() const noexcept {
        return frame_;
    }
    [[nodiscard]] const ScopeCaptureConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] std::uint64_t samples_seen() const noexcept {
        return samples_seen_;
    }
    [[nodiscard]] std::uint64_t holdoff_remaining() const noexcept {
        return holdoff_remaining_;
    }
    [[nodiscard]] std::size_t capture_completion_latency_samples() const noexcept {
        return config_.capture_samples - config_.pretrigger_samples - 1;
    }

  private:
    enum class State : std::uint8_t { armed, capturing, holdoff, stopped };

    static bool representable_(double value) noexcept {
        return value >= static_cast<double>(std::numeric_limits<SampleType>::lowest()) &&
               value <= static_cast<double>(std::numeric_limits<SampleType>::max());
    }

    void process_sample_(SampleType input) noexcept {
        const bool finite = std::isfinite(input);
        const SampleType sample = finite ? input : SampleType{0};
        if (!finite)
            ++nonfinite_samples_seen_;

        if (state_ == State::capturing) {
            workspace_[capture_write_++] = sample;
            if (capture_write_ == config_.capture_samples)
                complete_capture_();
            ++samples_seen_;
            return;
        }

        if (state_ == State::holdoff) {
            if (holdoff_remaining_ > 0)
                --holdoff_remaining_;
            if (holdoff_remaining_ == 0) {
                state_ = State::armed;
                rising_primed_ = false;
                falling_primed_ = false;
            }
            push_history_(sample);
            ++samples_seen_;
            return;
        }

        if (state_ == State::armed) {
            const bool immediate = config_.trigger_mode == ScopeTriggerMode::immediate;
            const bool edge_triggered = finite && !immediate && triggered_(sample);
            if (history_count_ >= config_.pretrigger_samples &&
                (immediate || edge_triggered)) {
                begin_capture_(sample);
                ++samples_seen_;
                return;
            } else if (finite) {
                update_priming_(sample);
            }
        }

        push_history_(sample);
        ++samples_seen_;
    }

    void update_priming_(SampleType sample) noexcept {
        const auto low = static_cast<SampleType>(config_.trigger_level - config_.hysteresis);
        const auto high = static_cast<SampleType>(config_.trigger_level + config_.hysteresis);
        if (sample < low)
            rising_primed_ = true;
        if (sample > high)
            falling_primed_ = true;
    }

    bool triggered_(SampleType sample) noexcept {
        const auto low = static_cast<SampleType>(config_.trigger_level - config_.hysteresis);
        const auto high = static_cast<SampleType>(config_.trigger_level + config_.hysteresis);
        const bool rising = rising_primed_ && sample >= high;
        const bool falling = falling_primed_ && sample <= low;
        if (rising)
            rising_primed_ = false;
        if (falling)
            falling_primed_ = false;
        switch (config_.trigger_mode) {
        case ScopeTriggerMode::immediate:
            return true;
        case ScopeTriggerMode::rising_edge:
            return rising;
        case ScopeTriggerMode::falling_edge:
            return falling;
        case ScopeTriggerMode::either_edge:
            return rising || falling;
        }
        return false;
    }

    void begin_capture_(SampleType trigger_sample) noexcept {
        if (config_.pretrigger_samples > 0) {
            const auto history_end = history_count_ == MaxSamples ? MaxSamples : history_count_;
            const auto history_start =
                (history_write_ + MaxSamples - config_.pretrigger_samples) % MaxSamples;
            std::rotate(workspace_.begin(), workspace_.begin() + history_start,
                        workspace_.begin() + history_end);
        }
        workspace_[config_.pretrigger_samples] = trigger_sample;
        capture_trigger_sample_index_ = samples_seen_;
        capture_write_ = config_.pretrigger_samples + 1;
        state_ = State::capturing;
        if (capture_write_ == config_.capture_samples)
            complete_capture_();
    }

    void complete_capture_() noexcept {
        std::copy_n(workspace_.begin(), config_.capture_samples, frame_.samples.begin());
        frame_.sample_count = config_.capture_samples;
        frame_.trigger_offset = config_.pretrigger_samples;
        frame_.trigger_sample_index = capture_trigger_sample_index_;
        frame_.generation = ++generation_;
        frame_.nonfinite_samples_seen = nonfinite_samples_seen_;
        ready_ = true;
        history_count_ = config_.capture_samples;
        history_write_ = config_.capture_samples % MaxSamples;
        capture_write_ = 0;
        if (!config_.continuous) {
            state_ = State::stopped;
            return;
        }
        holdoff_remaining_ = config_.holdoff_samples;
        state_ = holdoff_remaining_ == 0 ? State::armed : State::holdoff;
        rising_primed_ = false;
        falling_primed_ = false;
    }

    void push_history_(SampleType sample) noexcept {
        workspace_[history_write_] = sample;
        history_write_ = (history_write_ + 1) % MaxSamples;
        history_count_ = std::min(history_count_ + 1, MaxSamples);
    }

    ScopeCaptureConfig config_{};
    std::array<SampleType, MaxSamples> workspace_{};
    Frame frame_{};
    State state_ = State::armed;
    std::size_t history_write_ = 0;
    std::size_t history_count_ = 0;
    std::size_t capture_write_ = 0;
    std::uint64_t holdoff_remaining_ = 0;
    std::uint64_t samples_seen_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t nonfinite_samples_seen_ = 0;
    std::uint64_t capture_trigger_sample_index_ = 0;
    bool rising_primed_ = false;
    bool falling_primed_ = false;
    bool ready_ = false;
};

using ScopeCapture = ScopeCaptureT<float>;
using ScopeCapture64 = ScopeCaptureT<double>;

} // namespace pulp::signal
