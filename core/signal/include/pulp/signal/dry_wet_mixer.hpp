#pragma once

// Dry/wet mix with optional latency compensation.
// Use to blend original (dry) signal with processed (wet) signal.

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace pulp::signal {

enum class MixCurve {
    Linear,      // Simple linear crossfade
    EqualPower   // Constant-power crossfade (sqrt curve)
};

class DryWetMixer {
public:
    /// Set the mix ratio (0.0 = fully dry, 1.0 = fully wet)
    void set_mix(float mix) { mix_ = std::clamp(mix, 0.0f, 1.0f); }
    float mix() const { return mix_; }

    /// Set the mixing curve type
    void set_curve(MixCurve curve) { curve_ = curve; }

    /// Set latency in samples for the wet path (compensates dry path delay)
    void set_wet_latency(int samples) {
        const int new_latency = std::max(0, samples);
        if (new_latency == latency_)
            return;

        latency_ = new_latency;
        delay_pos_ = 0;

        if (latency_ > 0 && max_channels_ > 0)
            delay_buffer_.assign(static_cast<size_t>(latency_ * max_channels_), 0.0f);
        else
            delay_buffer_.clear();

        for (auto& ch : dry_buffer_)
            std::fill(ch.begin(), ch.end(), 0.0f);
    }

    /// Prepare for processing
    void prepare(int max_channels, int /*max_block_size*/) {
        max_channels_ = std::max(0, max_channels);
        delay_pos_ = 0;
        if (latency_ > 0 && max_channels_ > 0)
            delay_buffer_.assign(static_cast<size_t>(latency_ * max_channels_), 0.0f);
        else
            delay_buffer_.clear();
        dry_buffer_.clear();
    }

    /// Push dry samples before processing (call before your wet processing)
    void push_dry(const float* const* channels, int num_channels, int num_samples) {
        if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
            dry_buffer_.clear();
            return;
        }

        if (latency_ <= 0) {
            // No latency compensation — just store the dry signal
            dry_buffer_.resize(static_cast<size_t>(num_channels));
            for (int ch = 0; ch < num_channels; ++ch) {
                dry_buffer_[ch].resize(static_cast<size_t>(num_samples));
                std::memcpy(dry_buffer_[ch].data(), channels[ch],
                           static_cast<size_t>(num_samples) * sizeof(float));
            }
            return;
        }

        // With latency compensation — delay the dry signal.
        // Process sample-by-sample (all channels per sample) so the delay
        // position advances once per frame, not once per channel.
        const int count = std::min(num_channels, max_channels_);
        dry_buffer_.resize(static_cast<size_t>(count));
        for (int ch = 0; ch < count; ++ch)
            dry_buffer_[ch].resize(static_cast<size_t>(num_samples));

        for (int i = 0; i < num_samples; ++i) {
            for (int ch = 0; ch < count; ++ch) {
                size_t idx = static_cast<size_t>(delay_pos_ * max_channels_ + ch);
                dry_buffer_[ch][i] = delay_buffer_[idx];
                delay_buffer_[idx] = channels[ch][i];
            }
            delay_pos_ = (delay_pos_ + 1) % latency_;
        }
    }

    /// Mix dry and wet signals, writing result to wet_channels (in-place)
    void mix_wet(float* const* wet_channels, int num_channels, int num_samples) {
        float dry_gain, wet_gain;
        compute_gains(dry_gain, wet_gain);

        for (int ch = 0; ch < num_channels && ch < static_cast<int>(dry_buffer_.size()); ++ch) {
            const int sample_count = std::min(num_samples, static_cast<int>(dry_buffer_[ch].size()));
            for (int i = 0; i < sample_count; ++i) {
                wet_channels[ch][i] = dry_buffer_[ch][i] * dry_gain +
                                      wet_channels[ch][i] * wet_gain;
            }
        }
    }

    void reset() {
        std::fill(delay_buffer_.begin(), delay_buffer_.end(), 0.0f);
        delay_pos_ = 0;
        for (auto& ch : dry_buffer_)
            std::fill(ch.begin(), ch.end(), 0.0f);
    }

private:
    float mix_ = 1.0f;
    MixCurve curve_ = MixCurve::Linear;
    int latency_ = 0;
    int max_channels_ = 2;
    int delay_pos_ = 0;
    std::vector<float> delay_buffer_;
    std::vector<std::vector<float>> dry_buffer_;

    void compute_gains(float& dry, float& wet) const {
        switch (curve_) {
            case MixCurve::Linear:
                dry = 1.0f - mix_;
                wet = mix_;
                break;
            case MixCurve::EqualPower:
                dry = std::cos(mix_ * 1.5707963f);  // pi/2
                wet = std::sin(mix_ * 1.5707963f);
                break;
        }
    }
};

}  // namespace pulp::signal
