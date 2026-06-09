#include <pulp/audio/instrument_envelope.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::audio {

bool AhdsrEnvelope::prepare(const AhdsrEnvelopeConfig& config) noexcept {
    if (!config_valid(config)) return false;

    config_ = config;
    attack_frames_ = seconds_to_frames(config.attack_seconds, config.sample_rate);
    hold_frames_ = seconds_to_frames(config.hold_seconds, config.sample_rate);
    decay_frames_ = seconds_to_frames(config.decay_seconds, config.sample_rate);
    release_frames_ = seconds_to_frames(config.release_seconds, config.sample_rate);
    reset();
    return true;
}

void AhdsrEnvelope::reset() noexcept {
    stage_ = EnvelopeStage::Idle;
    stage_frame_ = 0;
    value_ = 0.0f;
    release_start_ = 0.0f;
}

void AhdsrEnvelope::note_on() noexcept {
    if (attack_frames_ == 0) {
        value_ = 1.0f;
        advance_from_peak_stage(false);
        return;
    }
    value_ = 0.0f;
    enter_stage(EnvelopeStage::Attack);
}

void AhdsrEnvelope::note_off() noexcept {
    if (stage_ == EnvelopeStage::Idle) return;
    if (release_frames_ == 0 || value_ <= 0.0f) {
        reset();
        return;
    }

    release_start_ = value_;
    enter_stage(EnvelopeStage::Release);
}

float AhdsrEnvelope::next_sample() noexcept {
    switch (stage_) {
    case EnvelopeStage::Idle:
        value_ = 0.0f;
        break;
    case EnvelopeStage::Attack: {
        ++stage_frame_;
        value_ = static_cast<float>(
            std::min(1.0, static_cast<double>(stage_frame_) /
                          static_cast<double>(attack_frames_)));
        if (stage_frame_ >= attack_frames_) {
            value_ = 1.0f;
            advance_from_peak_stage(true);
        }
        break;
    }
    case EnvelopeStage::Hold:
        value_ = 1.0f;
        ++stage_frame_;
        if (stage_frame_ >= hold_frames_) {
            if (decay_frames_ == 0) {
                enter_stage(EnvelopeStage::Sustain);
            } else {
                enter_stage(EnvelopeStage::Decay);
            }
        }
        break;
    case EnvelopeStage::Decay: {
        ++stage_frame_;
        const auto t = std::min(1.0,
                                static_cast<double>(stage_frame_) /
                                    static_cast<double>(decay_frames_));
        const auto level = 1.0 + (config_.sustain_level - 1.0) * t;
        value_ = static_cast<float>(level);
        if (stage_frame_ >= decay_frames_) {
            value_ = static_cast<float>(config_.sustain_level);
            enter_stage(EnvelopeStage::Sustain);
        }
        break;
    }
    case EnvelopeStage::Sustain:
        value_ = static_cast<float>(config_.sustain_level);
        break;
    case EnvelopeStage::Release: {
        ++stage_frame_;
        const auto t = std::min(1.0,
                                static_cast<double>(stage_frame_) /
                                    static_cast<double>(release_frames_));
        value_ = static_cast<float>(release_start_ * (1.0 - t));
        if (stage_frame_ >= release_frames_) {
            reset();
        }
        break;
    }
    }

    return value_;
}

void AhdsrEnvelope::render(std::span<float> destination) noexcept {
    for (auto& sample : destination) {
        sample = next_sample();
    }
}

bool AhdsrEnvelope::config_valid(const AhdsrEnvelopeConfig& config) noexcept {
    return config.sample_rate > 0.0 &&
           std::isfinite(config.sample_rate) &&
           config.attack_seconds >= 0.0 &&
           config.hold_seconds >= 0.0 &&
           config.decay_seconds >= 0.0 &&
           config.release_seconds >= 0.0 &&
           std::isfinite(config.attack_seconds) &&
           std::isfinite(config.hold_seconds) &&
           std::isfinite(config.decay_seconds) &&
           std::isfinite(config.release_seconds) &&
           config.sustain_level >= 0.0 &&
           config.sustain_level <= 1.0 &&
           std::isfinite(config.sustain_level) &&
           duration_frames_valid(config.attack_seconds, config.sample_rate) &&
           duration_frames_valid(config.hold_seconds, config.sample_rate) &&
           duration_frames_valid(config.decay_seconds, config.sample_rate) &&
           duration_frames_valid(config.release_seconds, config.sample_rate);
}

bool AhdsrEnvelope::duration_frames_valid(double seconds,
                                          double sample_rate) noexcept {
    const auto frames = seconds * sample_rate;
    return std::isfinite(frames) &&
           frames <= static_cast<double>(std::numeric_limits<std::uint64_t>::max());
}

std::uint64_t AhdsrEnvelope::seconds_to_frames(double seconds,
                                               double sample_rate) noexcept {
    if (seconds <= 0.0 || sample_rate <= 0.0) return 0;
    return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

void AhdsrEnvelope::enter_stage(EnvelopeStage stage) noexcept {
    stage_ = stage;
    stage_frame_ = 0;
}

void AhdsrEnvelope::advance_from_peak_stage(bool preserve_current_value) noexcept {
    if (hold_frames_ > 0) {
        enter_stage(EnvelopeStage::Hold);
    } else if (decay_frames_ > 0) {
        enter_stage(EnvelopeStage::Decay);
    } else {
        if (!preserve_current_value) {
            value_ = static_cast<float>(config_.sustain_level);
        }
        enter_stage(EnvelopeStage::Sustain);
    }
}

}  // namespace pulp::audio
