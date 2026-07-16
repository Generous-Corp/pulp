#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::audio {

enum class SampleStarvationMode : std::uint8_t {
    Ready,
    FadingOut,
    Silent,
    Recovering,
};

struct SampleStarvationEnvelopeConfig {
    std::uint32_t fade_out_frames = 0;
    std::uint32_t recovery_frames = 0;
};

struct SampleStarvationEnvelopeStats {
    std::uint64_t predicted_events = 0;
    std::uint64_t emergency_events = 0;
    std::uint64_t starved_frames = 0;
    std::uint64_t recovery_events = 0;
};

class SampleStarvationEnvelope {
public:
    bool prepare(const SampleStarvationEnvelopeConfig& config) noexcept {
        reset();
        if (config.fade_out_frames == 0 || config.recovery_frames == 0) return false;
        config_ = config;
        prepared_ = true;
        return true;
    }

    void reset() noexcept {
        config_ = {};
        stats_ = {};
        mode_ = SampleStarvationMode::Ready;
        ramp_frames_ = 0;
        ramp_position_ = 0;
        prepared_ = false;
    }

    bool prepared() const noexcept { return prepared_; }
    SampleStarvationMode mode() const noexcept { return mode_; }

    void begin_predicted_fade(std::uint32_t available_valid_frames) noexcept {
        if (!prepared_ || mode_ != SampleStarvationMode::Ready) {
            return;
        }
        ++stats_.predicted_events;
        ramp_frames_ = std::min(config_.fade_out_frames, available_valid_frames);
        ramp_position_ = 0;
        mode_ = ramp_frames_ == 0
            ? SampleStarvationMode::Silent
            : SampleStarvationMode::FadingOut;
    }

    float next_valid_gain() noexcept {
        if (!prepared_) return 0.0f;
        if (mode_ == SampleStarvationMode::Ready) return 1.0f;
        if (mode_ == SampleStarvationMode::Silent) return 0.0f;
        if (mode_ == SampleStarvationMode::Recovering) return next_recovery_gain();

        const float gain = equal_power_fade_out(ramp_position_, ramp_frames_);
        if (++ramp_position_ >= ramp_frames_) {
            mode_ = SampleStarvationMode::Silent;
        }
        return gain;
    }

    void mark_starved(std::uint64_t frames) noexcept {
        if (!prepared_) return;
        if (mode_ != SampleStarvationMode::Silent) ++stats_.emergency_events;
        stats_.starved_frames += frames;
        mode_ = SampleStarvationMode::Silent;
        ramp_frames_ = 0;
        ramp_position_ = 0;
    }

    void begin_recovery() noexcept {
        if (!prepared_ || mode_ != SampleStarvationMode::Silent) return;
        ++stats_.recovery_events;
        ramp_frames_ = config_.recovery_frames;
        ramp_position_ = 0;
        mode_ = SampleStarvationMode::Recovering;
    }

    SampleStarvationEnvelopeStats stats() const noexcept { return stats_; }

private:
    static float ramp_fraction(std::uint32_t position,
                               std::uint32_t frames) noexcept {
        if (frames <= 1) return 1.0f;
        return static_cast<float>(position) / static_cast<float>(frames - 1);
    }

    static float equal_power_fade_out(std::uint32_t position,
                                      std::uint32_t frames) noexcept {
        constexpr float kHalfPi = 1.57079632679489661923f;
        return std::cos(kHalfPi * ramp_fraction(position, frames));
    }

    float next_recovery_gain() noexcept {
        constexpr float kHalfPi = 1.57079632679489661923f;
        const float gain = std::sin(kHalfPi * ramp_fraction(ramp_position_, ramp_frames_));
        if (++ramp_position_ >= ramp_frames_) {
            mode_ = SampleStarvationMode::Ready;
        }
        return gain;
    }

    SampleStarvationEnvelopeConfig config_{};
    SampleStarvationEnvelopeStats stats_{};
    SampleStarvationMode mode_ = SampleStarvationMode::Ready;
    std::uint32_t ramp_frames_ = 0;
    std::uint32_t ramp_position_ = 0;
    bool prepared_ = false;
};

}  // namespace pulp::audio
