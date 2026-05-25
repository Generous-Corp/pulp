#pragma once

// Dry/wet mix with optional latency compensation.
// Use to blend original (dry) signal with processed (wet) signal.

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace pulp::signal {

/// Crossfade curve between dry (mix=0) and wet (mix=1).
///
/// Conventional crossfade taxonomy. `Sin3dB` is an alias of
/// `EqualPower` kept for explicit naming. Each curve hits dry=1/wet=0
/// at mix=0 and dry=0/wet=1 at mix=1; they differ in the midpoint
/// behavior:
/// - `Linear`:     -6 dB notch at midpoint, constant amplitude sum.
/// - `EqualPower`: -3 dB notch, constant power sum (sin/cos).
/// - `Balanced`:   dry stays at 1 until mix=0.5 then linearly drops
///                 to 0; wet inverse — useful for "include dry until
///                 midway" balance behavior.
/// - `Sin3dB`:     alias of EqualPower.
/// - `Sin4_5dB`:   sin/cos law shaped by exponent 1.5 → -4.5 dB notch.
/// - `Sin6dB`:     sin/cos law squared → -6 dB notch.
/// - `Sqrt3dB`:    sqrt law → -3 dB notch.
/// - `Sqrt4_5dB`:  sqrt with offset → -4.5 dB notch.
enum class MixCurve {
    Linear,
    EqualPower,
    Balanced,
    Sin3dB,
    Sin4_5dB,
    Sin6dB,
    Sqrt3dB,
    Sqrt4_5dB,
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
        constexpr float kHalfPi = 1.57079632679489661923f;
        const float theta = mix_ * kHalfPi;
        switch (curve_) {
            case MixCurve::Linear:
                dry = 1.0f - mix_;
                wet = mix_;
                break;
            case MixCurve::EqualPower:
            case MixCurve::Sin3dB:
                dry = std::cos(theta);
                wet = std::sin(theta);
                break;
            case MixCurve::Balanced:
                dry = mix_ <= 0.5f ? 1.0f : (1.0f - mix_) * 2.0f;
                wet = mix_ >= 0.5f ? 1.0f : mix_ * 2.0f;
                break;
            case MixCurve::Sin4_5dB: {
                // Clamp before pow to guard against tiny negative residue.
                const float c = std::max(0.0f, std::cos(theta));
                const float s = std::max(0.0f, std::sin(theta));
                dry = std::pow(c, 1.5f);
                wet = std::pow(s, 1.5f);
                break;
            }
            case MixCurve::Sin6dB:
                dry = std::cos(theta) * std::cos(theta);
                wet = std::sin(theta) * std::sin(theta);
                break;
            case MixCurve::Sqrt3dB:
                dry = std::sqrt(1.0f - mix_);
                wet = std::sqrt(mix_);
                break;
            case MixCurve::Sqrt4_5dB:
                // -4.5 dB = geometric mean of -3 dB sqrt (exp 0.5) and
                // -6 dB linear (exp 1.0) → exponent 0.75. At mix=0.5
                // produces 0.5^0.75 ≈ 0.5946 per side ≈ -4.51 dB,
                // matching the documented midpoint notch.
                dry = std::pow(1.0f - mix_, 0.75f);
                wet = std::pow(mix_, 0.75f);
                break;
        }
    }
};

}  // namespace pulp::signal
